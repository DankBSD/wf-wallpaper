void mainImage(out vec4 fragColor, in vec2 fragCoord) {
	vec2 uv = gl_FragCoord.xy/iResolution.xy;
	vec3 col = 0.5 + 0.5*cos(0.0+uv.xyx+vec3(0.0,2.0,4.0));
	fragColor = vec4(col,1.0);
}
