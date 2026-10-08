#if defined(BACKEND_GL_ES)
precision highp int;
precision highp float;
#endif

FRAGMENT_VARYING_IN(2) vec2 v_texcoord0;

DECLARE_FRAGMENT_COLOR_OUTPUT

// Group 1
UNIFORM(1,0) float u_time;
UNIFORM(1,1) mat4 u_projection;
UNIFORM(1,2) mat4 u_modelview;
UNIFORM(1,3) mat3 u_texcoord0_transform;
UNIFORM(1,4) vec2 u_texcoord0_min;
UNIFORM(1,5) vec2 u_texcoord0_max;
UNIFORM(1,6) int u_postimg_water;
UNIFORM(1,7) int u_postimg_heat;

UNIFORM(0,0) sampler2D s_sampler0; // screen
#ifdef ENABLE_S_SAMPLER1
UNIFORM(0,1) sampler2D s_sampler1; // palette
#endif

void main()
{
	float time = u_time;
	int water = u_postimg_water;
	int heat = u_postimg_heat;

	vec2 texcoord0 = v_texcoord0;
	vec2 sampler0_uvsize = u_texcoord0_max - u_texcoord0_min;

	int reclamp_uvs = 0;
	if (u_postimg_water > 0)
	{
		texcoord0.x += (sin((u_time / 35.0) * 2.0 + ((texcoord0.y - u_texcoord0_min.y) / sampler0_uvsize.y) * 20.0) * 0.01);
		reclamp_uvs = 1;
	}
	if (u_postimg_heat > 0)
	{
		texcoord0.x += (max(sin((u_time / 35.0) * 1.2 - ((texcoord0.y - u_texcoord0_min.y) / sampler0_uvsize.y) * 5.0) - 0.995, 0.0));
		texcoord0.x -= (max(sin(((u_time + 24.0) / 35.0) * 0.2 - ((texcoord0.y - u_texcoord0_min.y) / sampler0_uvsize.y) * 1.5) - 0.990, 0.0));
		reclamp_uvs = 1;
	}

	if (reclamp_uvs > 0)
	{
		texcoord0 = max(min(texcoord0, u_texcoord0_max - vec2(0.0001)), u_texcoord0_min);
	}

#ifdef ENABLE_S_SAMPLER1
	vec4 index_color = TEXTURE2D(s_sampler0, texcoord0);
	OUTPUT_COLOR = TEXTURE2D(s_sampler1, vec2(floor((index_color.r * 255.0) + 0.5) / 255.0, 0));
#else
	OUTPUT_COLOR = TEXTURE2D(s_sampler0, texcoord0);
#endif
}
