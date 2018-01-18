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

#ifdef __SHADOW_RECORD_ALL__

/* Shadow function to compute how much light is blocked, CPU variation.
*
* We trace a single ray. If it hits any opaque surface, or more than a given
* number of transparent surfaces is hit, then we consider the geometry to be
* entirely blocked. If not, all transparent surfaces will be recorded and we
* will shade them one by one to determine how much light is blocked. This all
* happens in one scene intersection function.
*
* Recording all hits works well in some cases but may be slower in others. If
* we have many semi-transparent hairs, one intersection may be faster because
* you'd be reinteresecting the same hairs a lot with each step otherwise. If
* however there is mostly binary transparency then we may be recording many
* unnecessary intersections when one of the first surfaces blocks all light.
*
* From tests in real scenes it seems the performance loss is either minimal,
* or there is a performance increase anyway due to avoiding the need to send
* two rays with transparent shadows.
*
* This is CPU only because of qsort, and malloc or high stack space usage to
* record all these intersections. */

#define STACK_MAX_HITS 64

 ccl_device_inline bool shadow_blocked_simple(KernelGlobals *kg, ShaderData *shadow_sd, LightSample *ls, PathState *state, Ray *ray, float3 *shadow, uint shadow_linking, Transform *shadow_map_tfm, int shadow_map_resolution, int shadow_map_slot)
{
	*shadow = make_float3(1.0f, 1.0f, 1.0f);

	if (ray->t == 0.0f)
		return false;

    if (shadow_map_slot >= 0) {
        // Project into shadowmap space
        float3 sm_coords = transform_perspective(shadow_map_tfm, ray->P);
        //sm_coords = sm_coords / sm_coords.w;

        if (sm_coords.x < 0.0F || sm_coords.x > 1.0F || sm_coords.y < 0.0F || sm_coords.y > 1.0F)
            return false;

        float4 sm_depth = svm_image_texture(kg, shadow_map_slot, sm_coords.x, sm_coords.y, false, false);

        // If we have not computed depth before, then do it!
        if (sm_depth.z == 0.0F) {

            // Calculate a new ray from light
            Ray sm_ray;
            sm_ray.P = ls->P;
            sm_ray.D = ray->P - ls->P;
            sm_ray.D = normalize_len(sm_ray.D, &sm_ray.t);
            sm_ray.t = FLT_MAX;
            sm_ray.dP = differential3_zero();
            sm_ray.dD = differential3_zero();

            bool blocked;
            Intersection isect;
            blocked = scene_intersect(kg, sm_ray, PATH_RAY_SHADOW_OPAQUE, &isect, NULL, 0.0f, 0.0f, shadow_linking);

            if (blocked) {
                sm_depth.z = isect.t;
            } else {
                sm_depth.z = FLT_MAX;
            }

            // Use sm_depth
            kernel_tex_image_write(shadow_map_slot,sm_coords.x,sm_coords.y,sm_depth);
        }

        return (sm_depth.z <= ray->t);

    } else {
        bool blocked;

        Intersection isect;
        blocked = scene_intersect(kg, *ray, PATH_RAY_SHADOW_OPAQUE, &isect, NULL, 0.0f, 0.0f, shadow_linking);

        return blocked;
    }
}

ccl_device_inline bool shadow_blocked(KernelGlobals *kg, ShaderData *shadow_sd, PathState *state, Ray *ray, float3 *shadow, uint shadow_linking)
{
	*shadow = make_float3(1.0f, 1.0f, 1.0f);

	if (ray->t == 0.0f)
		return false;

	bool blocked;

	if (kernel_data.integrator.transparent_shadows) {
		/* check transparent bounces here, for volume scatter which can do
		* lighting before surface path termination is checked */
		if (state->transparent_bounce >= kernel_data.integrator.transparent_max_bounce)
			return true;

		/* intersect to find an opaque surface, or record all transparent surface hits */
		Intersection hits_stack[STACK_MAX_HITS];
		Intersection *hits = hits_stack;
		const int transparent_max_bounce = kernel_data.integrator.transparent_max_bounce;
		uint max_hits = transparent_max_bounce - state->transparent_bounce - 1;

#    ifndef __KERNEL_GPU__
		/* prefer to use stack but use dynamic allocation if too deep max hits
		* we need max_hits + 1 storage space due to the logic in
		* scene_intersect_shadow_all which will first store and then check if
		* the limit is exceeded */
		if (max_hits + 1 > STACK_MAX_HITS) {
			if (kg->transparent_shadow_intersections == NULL) {
				kg->transparent_shadow_intersections =
					(Intersection*)malloc(sizeof(Intersection)*(transparent_max_bounce + 1));
			}
			hits = kg->transparent_shadow_intersections;
		}
#	  endif /*__KERNEL_GPU__*/

		uint num_hits;
		blocked = scene_intersect_shadow_all(kg, ray, hits, max_hits, &num_hits, shadow_linking);

		/* if no opaque surface found but we did find transparent hits, shade them */
		if (!blocked && num_hits > 0) {
			float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
			float3 Pend = ray->P + ray->D*ray->t;
			float last_t = 0.0f;
			int bounce = state->transparent_bounce;
			Intersection *isect = hits;
#ifdef __VOLUME__
			PathState ps = *state;
#endif
#	ifndef __KERNEL_GPU__
			qsort(hits, num_hits, sizeof(Intersection), intersections_compare);
#	endif /*__KERNEL_GPU__*/

			for (int hit = 0; hit < num_hits; hit++, isect++) {
				/* adjust intersection distance for moving ray forward */
				float new_t = isect->t;
				isect->t -= last_t;

				/* skip hit if we did not move forward, step by step raytracing
				* would have skipped it as well then */
				if (last_t == new_t)
					continue;

				last_t = new_t;

#ifdef __VOLUME__
				/* attenuation between last surface and next surface */
				if (ps.volume_stack[0].shader != SHADER_NONE) {
					Ray segment_ray = *ray;
					segment_ray.t = isect->t;
					kernel_volume_shadow(kg, shadow_sd, &ps, &segment_ray, &throughput);
				}
#endif

				/* setup shader data at surface */
				ShaderData sd;
				//shader_setup_from_ray(kg, shadow_sd, isect, ray);
				shader_setup_from_ray(kg, &sd, isect, ray);

				/* attenuation from transparent surface */
				if (!(/*shadow_sd->*/sd.shader_flag & SD_SHADER_HAS_ONLY_VOLUME)) {
					if ((/*shadow_sd->*/sd.shader_flag & SD_SHADER_USE_UNIFORM_ALPHA) && (!(/*shadow_sd->*/sd.shader_flag & SD_SHADER_USE_UNIFORM_ALPHA_SELF_ONLY) || (sd.shader == shadow_sd->shader))) {
						if (state->flag & PATH_RAY_AO)
							throughput *= (1.0f - /*shadow_sd->*/sd.ao_alpha);
						else
							throughput *= (1.0f - /*shadow_sd->*/sd.shadow_alpha);
					}
					else {
						path_state_modify_bounce(state, true);
						shader_eval_surface(kg, &sd/*shadow_sd*/, NULL, state, 0.0f, PATH_RAY_SHADOW, SHADER_CONTEXT_SHADOW, NULL, 0);
						path_state_modify_bounce(state, false);

						throughput *= shader_bsdf_transparency(kg, &sd/*shadow_sd*/);
					}
				}

				/* stop if all light is blocked */
				if (is_zero(throughput)) {
					return true;
				}

				/* move ray forward */
				//ray->P = shadow_sd->P;
				ray->P = sd.P;
				if (ray->t != FLT_MAX) {
					ray->D = normalize_len(Pend - ray->P, &ray->t);
				}

#ifdef __VOLUME__
				/* exit/enter volume */
				kernel_volume_stack_enter_exit(kg, /*shadow_sd*/&sd, ps.volume_stack);
#endif

				bounce++;
			}

#ifdef __VOLUME__
			/* attenuation for last line segment towards light */
			if (ps.volume_stack[0].shader != SHADER_NONE)
				kernel_volume_shadow(kg, shadow_sd, &ps, ray, &throughput);
#endif

			*shadow = throughput;

			return is_zero(throughput);
		}
	}
	else {
		Intersection isect;
		blocked = scene_intersect(kg, *ray, PATH_RAY_SHADOW_OPAQUE, &isect, NULL, 0.0f, 0.0f, shadow_linking);
	}

#ifdef __VOLUME__
	if (!blocked && state->volume_stack[0].shader != SHADER_NONE) {
		/* apply attenuation from current volume shader */
		kernel_volume_shadow(kg, shadow_sd, state, ray, shadow);
	}
#endif

	return blocked;
}

#undef STACK_MAX_HITS

#else

ccl_device_noinline bool shadow_blocked_simple(KernelGlobals *kg,
											   ShaderData *shadow_sd,
											   ccl_addr_space PathState *state,
											   ccl_addr_space Ray *ray_input,
											   float3 *shadow,
											   uint shadow_linking,
                                               Transform shadow_map_tfm,
                                               int shadow_map_resolution,
                                               int shadow_map_slot)
{
	*shadow = make_float3(1.0f, 1.0f, 1.0f);

#ifdef __SPLIT_KERNEL__
	Ray private_ray = *ray_input;
	Ray *ray = &private_ray;
#else
	Ray *ray = ray_input;
#endif

#ifdef __SPLIT_KERNEL__
	Intersection *isect = &kg->isect_shadow[SD_THREAD];
#else
	Intersection isect_object;
	Intersection *isect = &isect_object;
#endif

	if(ray_input->t == 0.0f)
		return false;
	bool blocked = scene_intersect(kg, *ray, PATH_RAY_SHADOW_OPAQUE, isect, NULL, 0.0f, 0.0f, shadow_linking);
	return blocked;
}

/* Shadow function to compute how much light is blocked, GPU variation.
*
* Here we raytrace from one transparent surface to the next step by step.
* To minimize overhead in cases where we don't need transparent shadows, we
* first trace a regular shadow ray. We check if the hit primitive was
* potentially transparent, and only in that case start marching. this gives
* one extra ray cast for the cases were we do want transparency. */

ccl_device_noinline bool shadow_blocked(KernelGlobals *kg,
										ShaderData *shadow_sd,
										ccl_addr_space PathState *state,
										ccl_addr_space Ray *ray_input,
										float3 *shadow,
										uint shadow_linking)
{
	*shadow = make_float3(1.0f, 1.0f, 1.0f);

	if (ray_input->t == 0.0f)
		return false;

#ifdef __SPLIT_KERNEL__
	Ray private_ray = *ray_input;
	Ray *ray = &private_ray;
#else
	Ray *ray = ray_input;
#endif

	Intersection isect_object;
	Intersection *isect = &isect_object;

	bool blocked = scene_intersect(kg, *ray, PATH_RAY_SHADOW_OPAQUE, isect, NULL, 0.0f, 0.0f, shadow_linking);

#ifdef __TRANSPARENT_SHADOWS__
	if (blocked && kernel_data.integrator.transparent_shadows) {
		if (shader_transparent_shadow(kg, isect)) {
			float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
			float3 Pend = ray->P + ray->D*ray->t;
			int bounce = state->transparent_bounce;
#ifdef __VOLUME__
			PathState ps = *state;
#endif

			for (;;) {
				if (bounce >= kernel_data.integrator.transparent_max_bounce)
					return true;

				if (!scene_intersect(kg, *ray, PATH_RAY_SHADOW_TRANSPARENT, isect, NULL, 0.0f, 0.0f, shadow_linking))
				{
#ifdef __VOLUME__
					/* attenuation for last line segment towards light */
					if (ps.volume_stack[0].shader != SHADER_NONE) {
						kernel_volume_shadow(kg, shadow_sd, &ps, ray, &throughput);
					}
#endif

					*shadow *= throughput;

					return false;
				}

				if (!shader_transparent_shadow(kg, isect)) {
					return true;
				}

#ifdef __VOLUME__
				/* attenuation between last surface and next surface */
				if (ps.volume_stack[0].shader != SHADER_NONE) {
					Ray segment_ray = *ray;
					segment_ray.t = isect->t;
					kernel_volume_shadow(kg, shadow_sd, &ps, &segment_ray, &throughput);
				}
#endif

				ShaderData *sd;
				shader_setup_from_ray(kg, sd, isect, ray);

				/* setup shader data at surface */
				//shader_setup_from_ray(kg, shadow_sd, isect, ray);

				/* attenuation from transparent surface */
				if (!(ccl_fetch(shadow_sd, shader_flag) & SD_SHADER_HAS_ONLY_VOLUME)) {
					if ((ccl_fetch(shadow_sd, shader_flag) & SD_SHADER_USE_UNIFORM_ALPHA) && (!(ccl_fetch(shadow_sd, shader_flag) & SD_SHADER_USE_UNIFORM_ALPHA_SELF_ONLY) || (ccl_fetch(shadow_sd, shader) == ccl_fetch(sd, shader)))) {
						if (state->flag & PATH_RAY_AO)
							throughput *= (1.0f - ccl_fetch(shadow_sd, ao_alpha));
						else
							throughput *= (1.0f - ccl_fetch(shadow_sd, shadow_alpha));
					}
					else {
						path_state_modify_bounce(state, true);
						shader_eval_surface(kg, sd, NULL, state, 0.0f, PATH_RAY_SHADOW, SHADER_CONTEXT_SHADOW, NULL, 0);
						path_state_modify_bounce(state, false);

						throughput *= shader_bsdf_transparency(kg, shadow_sd);
					}
				}

				/* stop if all light is blocked */
				if (is_zero(throughput)) {
					return true;
				}

				/* move ray forward */
				ray->P = ray_offset(ccl_fetch(shadow_sd, P), -ccl_fetch(shadow_sd, Ng));
				if (ray->t != FLT_MAX) {
					ray->D = normalize_len(Pend - ray->P, &ray->t);
				}

#ifdef __VOLUME__
				/* exit/enter volume */
				kernel_volume_stack_enter_exit(kg, shadow_sd, ps.volume_stack);
#endif

				bounce++;
			}
		}
	}
#ifdef __VOLUME__
	else if (!blocked && state->volume_stack[0].shader != SHADER_NONE) {
		/* apply attenuation from current volume shader */
		kernel_volume_shadow(kg, shadow_sd, state, ray, shadow);
	}
#endif
#endif

	return blocked;
}

#endif

CCL_NAMESPACE_END
