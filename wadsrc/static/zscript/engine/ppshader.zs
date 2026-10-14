struct PPShader native
{
	native clearscope static void SetEnabled(string shaderName, bool enable);
	native clearscope static void SetUniform1f(string shaderName, string uniformName, float value);
	native clearscope static void SetUniform2f(string shaderName, string uniformName, vector2 value);
	native clearscope static void SetUniform3f(string shaderName, string uniformName, vector3 value);
	native clearscope static void SetUniform4f(string shaderName, string uniformName, vector4 value);
	native clearscope static void SetUniform1i(string shaderName, string uniformName, int value);

	// Colour grading for an area (modern renderer): crossfade to a LUT texture
	// (a 1024x32 strip) or to an exposure bias in EV. Reset returns to the level's
	// MAPINFO 'gradinglut' / 'exposurebias' (or r_grading_lut). A new level resets both.
	native clearscope static void SetGradingLUT(string lutName, double fadeSeconds = 1);
	native clearscope static void ResetGradingLUT(double fadeSeconds = 1);
	native clearscope static void SetExposureBias(double ev, double fadeSeconds = 1);
	native clearscope static void ResetExposureBias(double fadeSeconds = 1);
}
