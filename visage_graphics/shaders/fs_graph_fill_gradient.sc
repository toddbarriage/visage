$input v_coordinates, v_dimensions, v_shader_values, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

// Contour-relative vertical-gradient graph fill. Identical to fs_graph_fill EXCEPT the
// brush gradient is sampled by DEPTH BELOW THE CURVE (0 at the contour/crest, 1 at the
// baseline) instead of by screen position. A normal brush gradient ramps in fixed rect
// space and so cannot follow a moving curve; this one fades along the curve at every
// column. A solid brush still returns its single color (sampleGradient of a 1-stop
// gradient is constant), so this only changes 2+-stop gradient fills. See
// libs/s-series-ui/June-21-optimization/graphfillgradient-primitive-plan.md.

#include <shader_include.sh>

SAMPLER2D(s_gradient, 0);
SAMPLER2D(s_texture, 1);

uniform vec4 u_atlas_scale;

void main() {
  float center_y = v_shader_values.x * v_dimensions.y;
  float resolution = v_shader_values.y;
  vec2 start_data = v_shader_values.zw * u_atlas_scale.xy;

  vec2 percent = v_coordinates * 0.5 + vec2(0.5, 0.5);
  vec2 pos = percent * v_dimensions;

  vec2 sample_pos = start_data + vec2(u_atlas_scale.x * percent.x * resolution, 0.0);
  float line_y1 = v_dimensions.y * texture2D(s_texture, sample_pos + vec2(-0.0001, 0.0)).r;
  float line_y2 = v_dimensions.y * texture2D(s_texture, sample_pos + vec2(0.0001, 0.0)).r;
  float line_y = 0.5 * (line_y1 + line_y2);
  float adjust = abs(line_y1 - line_y2) / (v_dimensions.x * 0.0002);
  float fade = sqrt(1.0 + adjust * adjust);

  // Depth along the fill: 0 at the contour (line_y), 1 at the baseline (center_y).
  // Guard the near-zero span (silent columns) so short bars don't divide by ~0.
  float span = center_y - line_y;
  float depth = clamp((pos.y - line_y) / (abs(span) < 0.001 ? 0.001 : span), 0.0, 1.0);
  gl_FragColor = sampleGradient(s_gradient, v_gradient_texture_pos.xy, v_gradient_texture_pos.zw, depth);

  float mult = sign(pos.y - center_y);
  float alpha = 1.0 - smoothed(-0.5 * fade, 0.5 * fade, mult * (pos.y - line_y)) * smoothed(-0.5, 0.5, mult * (pos.y - center_y));
  gl_FragColor.a = gl_FragColor.a * alpha;
}
