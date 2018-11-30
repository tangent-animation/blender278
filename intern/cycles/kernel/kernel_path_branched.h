/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

#ifdef __BRANCHED_PATH__

ccl_device_inline void kernel_branched_path_ao(KernelGlobals *kg,
                                               ShaderData *sd,
                                               ShaderData *emission_sd,
                                               PathRadiance *L,
                                               PathState *state,
                                               float3 throughput)
{
	int num_samples = kernel_data.integrator.ao_samples;
	float num_samples_inv = 1.0f/num_samples;
	float ao_factor = kernel_data.background.ao_factor;
	float3 ao_N;
	float3 ao_bsdf = shader_bsdf_ao(kg, sd, ao_factor, &ao_N);
	float3 ao_alpha = shader_bsdf_alpha(kg, sd);

	for(int j = 0; j < num_samples; j++) {
		float bsdf_u, bsdf_v;
		path_branched_rng_2D(kg, state->rng_hash, state, j, num_samples, PRNG_BSDF_U, &bsdf_u, &bsdf_v);

		float3 ao_D;
		float ao_pdf;

		sample_cos_hemisphere(ao_N, bsdf_u, bsdf_v, &ao_D, &ao_pdf);

		if(dot(sd->Ng, ao_D) > 0.0f && ao_pdf != 0.0f) {
			Ray light_ray;
			float3 ao_shadow;

			light_ray.P = ray_offset(sd->P, sd->Ng);
			light_ray.D = ao_D;
			light_ray.t = kernel_data.background.ao_distance;
#ifdef __OBJECT_MOTION__
			light_ray.time = ccl_fetch(sd, time);
#endif  /* __OBJECT_MOTION__ */
#ifdef __RAY_DIFFERENTIALS__
			light_ray.dP = ccl_fetch(sd, dP);
			/* This is how pbrt v3 implements differentials for diffuse bounces */
			float3 a, b;
			make_orthonormals(ao_D, &a, &b);
			light_ray.dD.dx = normalize(ao_D + 0.1f * a);
			light_ray.dD.dy = normalize(ao_D + 0.1f * b);
#endif

			state->flag |= PATH_RAY_AO;
			ShaderData sd_ao = *sd;
			shader_setup_from_ao_env(kg, &sd_ao, &light_ray);
			float3 ao_env = shader_eval_ao_env(kg, &sd_ao, state, 0, SHADER_CONTEXT_MAIN);

            uint shadow_linking = object_shadow_linking(kg, ccl_fetch(sd, object));
			if(!shadow_blocked(kg, sd, emission_sd, state, &light_ray, &ao_shadow, shadow_linking)) {
				path_radiance_accum_ao(L, state, throughput*num_samples_inv, ao_alpha, ao_bsdf * ao_env, ao_shadow, state->bounce);
			}
			else {
				path_radiance_accum_total_ao(L, state, throughput*num_samples_inv, ao_bsdf);
			}
			state->flag &= ~PATH_RAY_AO;
		}
	}
}


/* bounce off surface and integrate indirect light */
ccl_device_noinline void kernel_branched_path_surface_indirect_light(KernelGlobals *kg,
	ShaderData *sd, ShaderData *indirect_sd, ShaderData *emission_sd,
	float3 throughput, float num_samples_adjust, PathState *state, PathRadiance *L, uint light_linking){
	float sum_sample_weight;
	if(state->denoising_feature_weight > 0.0f) {
		sum_sample_weight = 0.0f;
		for(int i = 0; i < sd->num_closure; i++) {
			const ShaderClosure *sc = &sd->closure[i];

			if(!CLOSURE_IS_BSDF(sc->type) || CLOSURE_IS_BSDF_TRANSPARENT(sc->type)) {
				continue;
			}

			sum_sample_weight += sc->sample_weight;
		}
	}
	else {
		sum_sample_weight = 1.0f;
	}

	for (int i = 0; i < ccl_fetch(sd, num_closure); i++) {
		const ShaderClosure *sc = &ccl_fetch(sd, closure)[i];

		if(!CLOSURE_IS_BSDF(sc->type))
			continue;
		/* transparency is not handled here, but in outer loop */
		if(CLOSURE_IS_BSDF_TRANSPARENT(sc->type))
			continue;

		int num_samples;

		if (CLOSURE_IS_BSDF_DIFFUSE(sc->type))
			num_samples = (ccl_fetch(sd, shader_flag) & SD_SHADER_OVERRIDE_SAMPLES) ? ccl_fetch(sd, diffuse_samples) : kernel_data.integrator.diffuse_samples;
		else if (CLOSURE_IS_BSDF_BSSRDF(sc->type))
			num_samples = 1;
		else if (CLOSURE_IS_BSDF_GLOSSY(sc->type))
			num_samples = (ccl_fetch(sd, shader_flag) & SD_SHADER_OVERRIDE_SAMPLES) ? ccl_fetch(sd, glossy_samples) : kernel_data.integrator.glossy_samples;
		else
			num_samples = (ccl_fetch(sd, shader_flag) & SD_SHADER_OVERRIDE_SAMPLES) ? ccl_fetch(sd, transmission_samples) : kernel_data.integrator.transmission_samples;

		num_samples = ceil_to_int(num_samples_adjust*num_samples);

		float num_samples_inv = num_samples_adjust/num_samples;

		for(int j = 0; j < num_samples; j++) {
			PathState ps = *state;
			float3 tp = throughput;
			Ray bsdf_ray;

			ps.rng_hash = cmj_hash(state->rng_hash, i);

			if(!kernel_branched_path_surface_bounce(kg,
			                                        sd,
			                                        sc,
			                                        j,
			                                        num_samples,
			                                        &tp,
			                                        &ps,
			                                        L,
			                                        &bsdf_ray,
			                                        sum_sample_weight))
			{
				continue;
			}

			ps.rng_hash = state->rng_hash;

			kernel_path_indirect(kg,
			                     indirect_sd,
			                     emission_sd,
			                     &bsdf_ray,
			                     tp*num_samples_inv,
			                     num_samples,
			                     &ps,
			                     L,
                                 light_linking);

			/* for render passes, sum and reset indirect light pass variables
			 * for the next samples */
			path_radiance_sum_indirect(L);
			path_radiance_reset_indirect(L);
		}
	}
}

#ifdef __SUBSURFACE__
ccl_device void kernel_branched_path_subsurface_scatter(KernelGlobals *kg,
                                                        ShaderData *sd,
                                                        ShaderData *indirect_sd,
                                                        ShaderData *emission_sd,
                                                        PathRadiance *L,
                                                        PathState *state,
                                                        Ray *ray,
                                                        float3 throughput)
{
	for(int i = 0; i < sd->num_closure; i++) {
		ShaderClosure *sc = &sd->closure[i];

		if(!CLOSURE_IS_BSSRDF(sc->type))
			continue;

		/* set up random number generator */
		uint lcg_state = lcg_state_init(state, 0x68bc21eb);
		int num_samples = kernel_data.integrator.subsurface_samples;
		float num_samples_inv = 1.0f/num_samples;
		uint bssrdf_rng_hash = cmj_hash(state->rng_hash, i);

		/* do subsurface scatter step with copy of shader data, this will
		 * replace the BSSRDF with a diffuse BSDF closure */
		for(int j = 0; j < num_samples; j++) {
			SubsurfaceIntersection ss_isect;
			float bssrdf_u, bssrdf_v;
			path_branched_rng_2D(kg, bssrdf_rng_hash, state, j, num_samples, PRNG_BSDF_U, &bssrdf_u, &bssrdf_v);
			int num_hits = subsurface_scatter_multi_intersect(kg,
			                                                  &ss_isect,
			                                                  sd,
			                                                  sc,
			                                                  &lcg_state,
			                                                  bssrdf_u, bssrdf_v,
			                                                  true);
#ifdef __VOLUME__
			Ray volume_ray = *ray;
			bool need_update_volume_stack = kernel_data.integrator.use_volumes &&
			                                ccl_fetch(sd, object_flag) & SD_OBJECT_OBJECT_INTERSECTS_VOLUME;
#endif

			/* compute lighting with the BSDF closure */
			for(int hit = 0; hit < num_hits; hit++) {
				ShaderData bssrdf_sd = *sd;
				subsurface_scatter_multi_setup(kg,
				                               &ss_isect,
				                               hit,
				                               &bssrdf_sd,
				                               state,
				                               state->flag,
				                               sc,
				                               true);

                uint light_linking = object_light_linking(kg, bssrdf_sd.object);
                uint shadow_linking = object_shadow_linking(kg, bssrdf_sd.object);

				PathState hit_state = *state;

				path_state_branch(&hit_state, j, num_samples);

#ifdef __VOLUME__
				if(need_update_volume_stack) {
					/* Setup ray from previous surface point to the new one. */
					float3 P = ray_offset(bssrdf_sd.P, -bssrdf_sd.Ng);
					volume_ray.D = normalize_len(P - volume_ray.P,
					                             &volume_ray.t);

					kernel_volume_stack_update_for_subsurface(
					    kg,
					    emission_sd,
					    &volume_ray,
					    &hit_state);
				}
#endif  /* __VOLUME__ */

#ifdef __EMISSION__
				/* direct light */
				if(kernel_data.integrator.use_direct_light) {
					int all = (kernel_data.integrator.sample_all_lights_direct) ||
					          (state->flag & PATH_RAY_SHADOW_CATCHER);
					kernel_branched_path_surface_connect_light(
					        kg,
					        &bssrdf_sd,
					        emission_sd,
					        &hit_state,
					        throughput,
					        num_samples_inv,
					        L,
					        all,
                            light_linking,
                            shadow_linking);
				}
#endif  /* __EMISSION__ */

				/* indirect light */
				kernel_branched_path_surface_indirect_light(
				        kg,
				        &bssrdf_sd,
				        indirect_sd,
				        emission_sd,
				        throughput,
				        num_samples_inv,
				        &hit_state,
				        L,
                        light_linking);
			}
		}
	}
}
#endif  /* __SUBSURFACE__ */

ccl_device void kernel_branched_path_integrate(KernelGlobals *kg, uint rng_hash, int sample, Ray ray, ccl_global float *buffer)
{
	/* initialize */
	PathRadiance L;
	float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
	float L_transparent = 0.0f;

	path_radiance_init(&L, kernel_data.film.use_light_pass);

	/* shader data memory used for both volumes and surfaces, saves stack space */
	ShaderData sd;
	sd.P = ray.P;
	/* shader data used by emission, shadows, volume stacks, indirect path */
	ShaderData emission_sd, indirect_sd;

	PathState state;
	path_state_init(kg, &emission_sd, &state, rng_hash, sample, &ray);

	Ray volume_ray = ray;
	int volumes_entered = 0;

	/* is there a heterogenous world shader? */
	if(state.volume_stack[0].shader != SHADER_NONE && volume_stack_is_heterogeneous(kg, state.volume_stack)) {
		++volumes_entered;
	}

#ifdef __KERNEL_DEBUG__
	DebugData debug_data;
	debug_data_init(&debug_data);
#endif  /* __KERNEL_DEBUG__ */

	/* Main Loop
	 * Here we only handle transparency intersections from the camera ray.
	 * Indirect bounces are handled in kernel_branched_path_surface_indirect_light().
	 */
	for(;;) {
		/* intersect scene */
		Intersection isect;
		uint visibility = path_state_ray_visibility(kg, &state);

#ifdef __HAIR__
		float difl = 0.0f, extmax = 0.0f;
		uint lcg_state = 0;

		if(kernel_data.bvh.have_curves) {
			if(kernel_data.cam.resolution == 1) {
				float3 pixdiff = ray.dD.dx + ray.dD.dy;
				/*pixdiff = pixdiff - dot(pixdiff, ray.D)*ray.D;*/
				difl = kernel_data.curve.minimum_width * len(pixdiff) * 0.5f;
			}

			extmax = kernel_data.curve.maximum_width;
			lcg_state = lcg_state_init(&state, 0x51633e2d);
		}

		bool hit = scene_intersect(kg, ray, visibility, &isect, &lcg_state, difl, extmax, 0x00000000 /*TODO: What goes here*/);
#else
		bool hit = scene_intersect(kg, ray, visibility, &isect, NULL, 0.0f, 0.0f, 0x00000000 /*TODO: What goes here*/);
#endif

#ifdef __KERNEL_DEBUG__
		debug_data.num_bvh_traversed_nodes += isect.num_traversed_nodes;
		debug_data.num_bvh_traversed_instances += isect.num_traversed_instances;
		debug_data.num_bvh_intersections += isect.num_intersections;
		debug_data.num_ray_bounces++;
#endif  /* __KERNEL_DEBUG__ */

#ifdef __VOLUME__		/* volume attenuation, emission, scatter */
		/* this determines if we do volume tracing right away or defer until we hit a non-volume surface */
		bool do_volume = false;

		/* Do all homogenous volumes right away. */
		if(state.volume_stack[0].shader != SHADER_NONE && !volume_stack_is_heterogeneous(kg, state.volume_stack)) {
			do_volume = true;
		}

		if(hit) {
			shader_setup_from_ray(kg, &sd, &isect, &ray);
			if(sd.shader_flag & SD_SHADER_HAS_VOLUME) {
				if(state.volume_stack[0].shader == SHADER_NONE) {
					volume_ray.P = sd.P;
					volume_ray.t = 0.0f;
					volume_ray.D = ray.D;
				}
				else {
					volume_ray.t = len(sd.P - volume_ray.P);
				}
				if(sd.runtime_flag & SD_RUNTIME_BACKFACING) {
					/* The ray is leaving a volume. */
					--volumes_entered;
					for(int i = 0; state.volume_stack[i].shader != SHADER_NONE && i < (volume_stack_size(&state)-1); ++i) {
						if(state.volume_stack[i].object == sd.object && state.volume_stack[i].depth > 0) {
							--state.volume_stack[i].depth;
							if(state.volume_stack[i].t_exit == FLT_MAX) {
								state.volume_stack[i].t_exit = volume_ray.t;
							}
							else {
								/* The ray should traverse front-to-back, but sometimes it doesn't?! */
								assert(volume_ray.t >= state.volume_stack[i].t_exit);
								state.volume_stack[i].t_exit = max(volume_ray.t, state.volume_stack[i].t_exit);
							}
							break;
						}
					}
				}
				else {
					/* The ray is entering a volume. */
					int i = 0;
					++volumes_entered;
					bool found = false;
					while(state.volume_stack[i].shader != SHADER_NONE && i < (volume_stack_size(&state) - 1)) {
						if(state.volume_stack[i].object == sd.object) {
							bool inside = state.volume_stack[i].depth == 0 && volume_ray.t >= state.volume_stack[i].t_enter && volume_ray.t <= state.volume_stack[i].t_exit;
							/* The ray should traverse front-to-back, but sometimes it doesn't?! */
							assert(!inside);
							if(state.volume_stack[i].depth > 0 || inside) {
							/* This is a re-entry into an object we haven't left yet. */
							++state.volume_stack[i].depth;
							found = true;
							break;
							}
						}
						++i;
#ifdef __KERNEL_CPU__
						/* Grow volume stack if necessary. */
						if(i == volume_stack_size(&state) - 1) {
							state.volume_stack_storage.resize(state.volume_stack_storage.size() + VOLUME_STACK_SIZE);
							state.volume_stack = &state.volume_stack_storage[0];
						}
#endif
					}
					if(i < volume_stack_size(&state)-1 && !found) {
						state.volume_stack[i].object = sd.object;
						state.volume_stack[i].shader = sd.shader;
						state.volume_stack[i].t_enter = volume_ray.t;
						state.volume_stack[i].t_exit = FLT_MAX;
						state.volume_stack[i].depth = 1;
						state.volume_stack[i + 1].shader = SHADER_NONE;
					}
					else {
						/* Not enough room on the stack. Skip this object. */
						kernel_assert(found);
					}
				}
			}
		}
		else {
			int i = (kernel_data.background.volume_shader != SHADER_NONE) ? 1 : 0;
			 while(i < volume_stack_size(&state) && state.volume_stack[i].shader != SHADER_NONE) {
				if(state.volume_stack[i].t_exit != FLT_MAX) {
					++i;
				}
				else {
					kernel_volume_stack_remove(kg, state.volume_stack[i].object, state.volume_stack);
				}
			}
		}

		/* Collect heterogenous volume interactions until a non-volume object is intersected
		 or the ray leaves all volumes. Then do one ray march through all collected media.
		 */

		do_volume |= volumes_entered == 0 || (!hit) /* Leaving volumes or scene */
			|| (!(sd.shader_flag & SD_SHADER_HAS_ONLY_VOLUME)); /* hit a non-volume object */
		do_volume &= state.volume_stack[0].shader != SHADER_NONE;


		if(do_volume) {
			float3 save_p = sd.P;
			if(hit && !(sd.shader_flag & SD_SHADER_HAS_ONLY_VOLUME)) {
				volume_ray.t = len(sd.P - volume_ray.P);
			}
			else if(!hit) {
				if(kernel_data.background.volume_shader != SHADER_NONE) {
					volume_ray.t = FLT_MAX;
				}
			}

			bool heterogeneous = volume_stack_is_heterogeneous(kg, state.volume_stack);

#ifdef __VOLUME_DECOUPLED__
			/* decoupled ray marching only supported on CPU */

			/* cache steps along volume for repeated sampling */
			VolumeSegment volume_segment;

			shader_setup_from_volume(kg, &sd, &volume_ray);
			kernel_volume_decoupled_record(kg, &state,
				&volume_ray, &sd, &volume_segment, heterogeneous);

			/* direct light sampling */
			if(volume_segment.closure_flag & SD_RUNTIME_SCATTER) {
				volume_segment.sampling_method = volume_stack_sampling_method(kg, state.volume_stack);

				int all = kernel_data.integrator.sample_all_lights_direct;

                uint light_linking = object_light_linking(kg, sd.object);
                uint shadow_linking = object_shadow_linking(kg, sd.object);

				kernel_branched_path_volume_connect_light(kg, &sd,
					&emission_sd, throughput, &state, &L, all,
					&volume_ray, &volume_segment, light_linking, shadow_linking);

				/* indirect light sampling */
				int num_samples = kernel_data.integrator.volume_samples;
				float num_samples_inv = 1.0f/num_samples;

				for(int j = 0; j < num_samples; j++) {
					PathState ps = state;
					Ray pray = volume_ray;
					float3 tp = throughput;

					/* branch RNG state */
					path_state_branch(&ps, j, num_samples);

					/* scatter sample. if we use distance sampling and take just one
					 * sample for direct and indirect light, we could share this
					 * computation, but makes code a bit complex */
					float rphase = path_state_rng_1D_for_decision(kg, &ps, PRNG_PHASE);
					float rscatter = path_state_rng_1D_for_decision(kg, &ps, PRNG_SCATTER_DISTANCE);

					VolumeIntegrateResult result = kernel_volume_decoupled_scatter(kg,
						&ps, &pray, &sd, &tp, rphase, rscatter, &volume_segment, NULL, false);

					kernel_volume_branch_stack(sd.ray_length, &ps);

					(void)result;
					kernel_assert(result == VOLUME_PATH_SCATTERED);

					if(kernel_path_volume_bounce(kg,
					                             &sd,
					                             &tp,
					                             &ps,
					                             &L,
					                             &pray))
					{
						kernel_path_indirect(kg,
						                     &indirect_sd,
						                     &emission_sd,
						                     &pray,
						                     tp*num_samples_inv,
						                     num_samples,
						                     &ps,
						                     &L,
                                             light_linking);

						/* for render passes, sum and reset indirect light pass variables
						 * for the next samples */
						path_radiance_sum_indirect(&L);
						path_radiance_reset_indirect(&L);
					}
				}
			}

			/* emission and transmittance */
			if(volume_segment.closure_flag & SD_RUNTIME_EMISSION)
				path_radiance_accum_emission(&L, throughput, volume_segment.accum_emission, state.bounce);
			throughput *= volume_segment.accum_transmittance;

			/* free cached steps */
			kernel_volume_decoupled_free(kg, &volume_segment);
#else

			uint light_linking = object_light_linking(kg, sd.object);
			uint shadow_linking = object_shadow_linking(kg, sd.object);

			/* GPU: no decoupled ray marching, scatter probalistically */
			int num_samples = kernel_data.integrator.volume_samples;
			float num_samples_inv = 1.0f/num_samples;

			/* todo: we should cache the shader evaluations from stepping
			 * through the volume, for now we redo them multiple times */

			for(int j = 0; j < num_samples; j++) {
				PathState ps = state;
				Ray pray = volume_ray;
				float3 tp = throughput * num_samples_inv;

				/* branch RNG state */
				path_state_branch(&ps, j, num_samples);

				VolumeIntegrateResult result = kernel_volume_integrate(
					kg, &ps, &sd, &volume_ray, &L, &tp, heterogeneous);

				kernel_volume_branch_stack(sd.ray_length, &ps);

#ifdef __VOLUME_SCATTER__
				if(result == VOLUME_PATH_SCATTERED) {
					/* todo: support equiangular, MIS and all light sampling.
					 * alternatively get decoupled ray marching working on the GPU */
					kernel_path_volume_connect_light(kg, &sd, &emission_sd, tp, &state, &L, light_linking, shadow_linking);

					if(kernel_path_volume_bounce(kg,
					                             &sd,
					                             &tp,
					                             &ps,
					                             &L,
					                             &pray))
					{
						kernel_path_indirect(kg,
						                     &indirect_sd,
						                     &emission_sd,
						                     &pray,
						                     tp,
						                     num_samples,
						                     &ps,
						                     &L,
											 light_linking);

						/* for render passes, sum and reset indirect light pass variables
						 * for the next samples */
						path_radiance_sum_indirect(&L);
						path_radiance_reset_indirect(&L);
					}
				}
#endif  /* __VOLUME_SCATTER__ */
			}

			/* todo: avoid this calculation using decoupled ray marching */
			kernel_volume_shadow(kg, &emission_sd, &state, &volume_ray, &throughput);
#endif  /* __VOLUME_DECOUPLED__ */
			for(int i = 0; state.volume_stack[i].shader != SHADER_NONE && i < (volume_stack_size(&state)-1); ++i) {
				if(state.volume_stack[i].t_exit < FLT_MAX) {
					int j = i;
					/* shift back next stack entries */
					do {
						state.volume_stack[j] = state.volume_stack[j+1];
						++j;
					}
					while(state.volume_stack[j].shader != SHADER_NONE && j < volume_stack_size(&state));
					--i;
				}
				if(i > 0) {
					state.volume_stack[i].t_enter = 0.0f;
				}
			}
			volume_ray.P = save_p;
		}
#endif  /* __VOLUME__ */

		if(!hit) {
			/* eval background shader if nothing hit */
			if(kernel_data.background.transparent) {
				L_transparent += average(throughput);

#ifdef __PASSES__
				if(!(kernel_data.film.pass_flag & PASS_BACKGROUND))
#endif  /* __PASSES__ */
					break;
			}

#ifdef __BACKGROUND__
			/* sample background shader */
			float3 L_background = indirect_background(kg, &emission_sd, &state, &ray, buffer, sample);
			path_radiance_accum_background(&L, &state, throughput, L_background);
#endif  /* __BACKGROUND__ */

			break;
		}

		/* setup shading */
		shader_setup_from_ray(kg, &sd, &isect, &ray);

		/* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
		if (!(sd.shader_flag & SD_SHADER_HAS_ONLY_VOLUME)) {
#endif
		shader_eval_surface(kg, &sd, &state, 0.0f, state.flag, SHADER_CONTEXT_MAIN, buffer, sample);
		shader_merge_closures(&sd);

#ifdef __SHADOW_TRICKS__
		if((sd.object_flag & SD_OBJECT_OBJECT_SHADOW_CATCHER)) {
			if(state.flag & PATH_RAY_CAMERA) {
				state.flag |= (PATH_RAY_SHADOW_CATCHER | PATH_RAY_STORE_SHADOW_INFO | PATH_RAY_SHADOW_CATCHER_ONLY);
				state.catcher_object = sd.object;
				if(!kernel_data.background.transparent) {
					L.shadow_color = indirect_background(kg, &emission_sd, &state, &ray, buffer, sample);
				}
			}
		}
		else {
			state.flag &= ~PATH_RAY_SHADOW_CATCHER_ONLY;
		}
#endif  /* __SHADOW_TRICKS__ */

		/* holdout */
#ifdef __HOLDOUT__
		if((sd.runtime_flag & SD_RUNTIME_HOLDOUT) || (sd.object_flag & SD_OBJECT_HOLDOUT_MASK)) {
			if(kernel_data.background.transparent) {
				float3 holdout_weight;
				
				if(sd.object_flag & SD_OBJECT_HOLDOUT_MASK) {
					holdout_weight = make_float3(1.0f, 1.0f, 1.0f);
				}
				else {
					holdout_weight = shader_holdout_eval(kg, &sd);
				}
				/* any throughput is ok, should all be identical here */
				L_transparent += average(holdout_weight*throughput);
			}

			if(sd.object_flag & SD_OBJECT_HOLDOUT_MASK) {
				break;
			}
		}
#endif  /* __HOLDOUT__ */

		/* holdout mask objects do not write data passes */
		kernel_write_data_passes(kg, buffer, &L, &sd, sample, &state, throughput);

#ifdef __EMISSION__
		/* emission */
		if(sd.runtime_flag & SD_RUNTIME_EMISSION) {
			float3 emission = indirect_primitive_emission(kg, &sd, isect.t, state.flag, state.ray_pdf);
			path_radiance_accum_emission(&L, throughput, emission, state.bounce);
		}
#endif  /* __EMISSION__ */

		/* transparency termination */
		if(state.flag & PATH_RAY_TRANSPARENT) {
			/* path termination. this is a strange place to put the termination, it's
			 * mainly due to the mixed in MIS that we use. gives too many unneeded
			 * shader evaluations, only need emission if we are going to terminate */
			float probability = path_state_terminate_probability(kg, &state, &sd, throughput);

			if(probability == 0.0f) {
				break;
			}
			else if(probability != 1.0f) {
				float terminate = path_state_rng_1D_for_decision(kg, &state, PRNG_TERMINATE);

				if(terminate >= probability)
					break;

				throughput /= probability;
			}
		}

		kernel_update_denoising_features(kg, &sd, &state, &L);

#ifdef __AO__
		/* ambient occlusion */
		if(kernel_data.integrator.use_ambient_occlusion || (sd.runtime_flag & SD_RUNTIME_AO)) {
			kernel_branched_path_ao(kg, &sd, &emission_sd, &L, &state, throughput);
		}
#endif  /* __AO__ */

#ifdef __SUBSURFACE__
		/* bssrdf scatter to a different location on the same object */
		if(sd.runtime_flag & SD_RUNTIME_BSSRDF) {
			kernel_branched_path_subsurface_scatter(kg, &sd, &indirect_sd, &emission_sd,
			                                        &L, &state, &ray, throughput);
		}
#endif  /* __SUBSURFACE__ */

		PathState hit_state = state;

#ifdef __VOLUME__
		for(int i = 0; hit_state.volume_stack[i].shader != SHADER_NONE && i < (volume_stack_size(&state)-1); ++i) {
			hit_state.volume_stack[i].t_enter = 0.0f;
			hit_state.volume_stack[i].t_exit = FLT_MAX;
		}
#endif  /* __VOLUME__ */

        uint light_linking = object_light_linking(kg, sd.object);

#ifdef __EMISSION__
		/* direct light */
		if(kernel_data.integrator.use_direct_light) {
			int all = kernel_data.integrator.sample_all_lights_direct;

            uint shadow_linking = object_shadow_linking(kg, sd.object);

			kernel_branched_path_surface_connect_light(kg,
				&sd, &emission_sd, &hit_state, throughput, 1.0f, &L, all,
                light_linking, shadow_linking);
		}
#endif  /* __EMISSION__ */

		/* indirect light */
		kernel_branched_path_surface_indirect_light(kg,
			&sd, &indirect_sd, &emission_sd, throughput, 1.0f, &hit_state, &L, light_linking);

		/* continue in case of transparency */
		float3 transparency = shader_bsdf_transparency(kg, &sd);
		throughput *= transparency;

		if(is_zero(throughput))
			break;
			
		state.matte_weight *= average(transparency);

		/* Update Path State */
		state.flag |= PATH_RAY_TRANSPARENT;
		state.transparent_bounce++;

#ifdef __VOLUME__
		}
		else {
			if(!path_state_volume_next(kg, &state)) {
				break;
			}
		}
#endif

		ray.P = ray_offset(sd.P, -sd.Ng);
		ray.t -= sd.ray_length; /* clipping works through transparent */

#ifdef __RAY_DIFFERENTIALS__
		ray.dP = sd.dP;
		ray.dD.dx = -sd.dI.dx;
		ray.dD.dy = -sd.dI.dy;
#endif  /* __RAY_DIFFERENTIALS__ */
	}

#ifdef __KERNEL_DEBUG__
	kernel_write_debug_passes(kg, buffer, &state, &debug_data, sample);
#endif  /* __KERNEL_DEBUG__ */

	kernel_write_result(kg, buffer, sample, &L, L_transparent, state.flag & PATH_RAY_SHADOW_CATCHER);
}

ccl_device void kernel_branched_path_trace(KernelGlobals *kg,
	ccl_global float *buffer, ccl_global uint *rng_state,
	int sample, int x, int y, int offset, int stride)
{
	/* buffer offset */
	int index = offset + x + y*stride;
	int pass_stride = kernel_data.film.pass_stride;

	rng_state += index;
	buffer += index*pass_stride;

	/* initialize random numbers and ray */
	uint rng_hash;
	Ray ray;

	kernel_path_trace_setup(kg, rng_state, sample, x, y, &rng_hash, &ray);

	if(ray.t != 0.0f)
		kernel_branched_path_integrate(kg, rng_hash, sample, ray, buffer);
	else
		kernel_write_result(kg, buffer, sample, NULL, 0.0f, false);}

#endif  /* __BRANCHED_PATH__ */

CCL_NAMESPACE_END

