# ── sdk/xrender — 2D rendering subcomponent ───────────────────────────────────
# Included via subdir('sdk/xrender') from the root meson.build.
# Expects the following variables to exist in the parent scope:
#   freetype_dep, harfbuzz_dep, fontconfig_dep, libpng_dep, vulkan_dep, gtest_dep
# Exports:
#   libxrender  — static_library target
#   xrender_dep — declare_dependency wrapping libxrender

# Sources compiled into libfreebsdx-ui (app-facing subset of xrender)
xrender_app_srcs = files(
  'src/display_list.cc',
  'src/font_atlas.cc',
  'src/text_shaper.cc',
  'src/font_manager.cc',
  'src/image_decoder.cc',
  'src/svg_engine.cc',
)

# Include dirs (paths relative to sdk/xrender/)
xrender_inc = include_directories(
  'include',
  '../../include',
  '../../core/xserver/include',
)

libxrender = static_library('xrender',
  xrender_app_srcs,
  include_directories : xrender_inc,
  dependencies        : [freetype_dep, harfbuzz_dep, fontconfig_dep, libpng_dep, vulkan_dep],
)

xrender_dep = declare_dependency(
  link_with           : libxrender,
  include_directories : xrender_inc,
  dependencies        : [freetype_dep, harfbuzz_dep, fontconfig_dep, libpng_dep, vulkan_dep],
)

# ── meson unit tests ──────────────────────────────────────────────────────────
# Add new rendering unit tests here as tests/unit/*.cc are created.
if gtest_dep.found()
  # SVG rasterizer — geometry/fill/gradient correctness; guards against the
  # engine regressing back to the old "fill the whole box opaque" placeholder.
  test('xrender svg_engine',
    executable('xrender_svg_engine_test',
      'tests/unit/svg_engine_test.cc',
      include_directories : xrender_inc,
      dependencies        : [gtest_dep, xrender_dep]),
    suite : 'unit')
endif
