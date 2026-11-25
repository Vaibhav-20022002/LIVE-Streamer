project('live_streamer', 'cpp', version: '1.0.0', default_options: ['cpp_std=c++20', 'warning_level=2'])

fs = import('fs')
cpp = meson.get_compiler('cpp')
threads_dep = dependency('threads')

libav_names = ['avformat', 'avcodec', 'avutil', 'avdevice', 'swscale']
libav_deps = []
need_homebrew_inc = false

foreach lib : libav_names
  dep = dependency('lib' + lib, required: false)
  if dep.found()
    libav_deps += dep
  else
    lib_found = cpp.find_library(lib, dirs: ['/opt/homebrew/lib', '/usr/local/lib'], required: true)
    libav_deps += lib_found
    need_homebrew_inc = true
  endif
endforeach

inc_dirs = [include_directories('src')]

if need_homebrew_inc and fs.is_dir('/opt/homebrew/include')
  inc_dirs += include_directories('/opt/homebrew/include', is_system: true)
endif

if fs.is_dir('/usr/local/include')
  inc_dirs += include_directories('/usr/local/include', is_system: true)
endif

enable_ai_opt = get_option('enable_ai')
ai_enabled_flag = '-DENABLE_AI=0'
onnx_deps = []
onnx_inc = []

if not enable_ai_opt.disabled()
  ort_pkg = dependency('libonnxruntime', required: false)
  if ort_pkg.found()
    onnx_deps += ort_pkg
    ai_enabled_flag = '-DENABLE_AI=1'
  else
    roots = []
    custom_root = get_option('onnxruntime_dir')
    if custom_root != ''
      roots += custom_root
    else
      roots += ['/opt/onnxruntime', '/usr/local', '/opt/homebrew']
    endif

    foreach r : roots
      if fs.is_file(r / 'include/onnxruntime_cxx_api.h')
        ort_lib = cpp.find_library('onnxruntime', dirs: [r / 'lib'], required: false)
        if ort_lib.found()
          ai_enabled_flag = '-DENABLE_AI=1'
          onnx_deps += ort_lib
          onnx_inc += include_directories(r / 'include', is_system: true)
          break
        endif
      elif fs.is_file(r / 'include/onnxruntime/onnxruntime_cxx_api.h')
        ort_lib = cpp.find_library('onnxruntime', dirs: [r / 'lib'], required: false)
        if ort_lib.found()
          ai_enabled_flag = '-DENABLE_AI=1'
          onnx_deps += ort_lib
          onnx_inc += include_directories(r / 'include/onnxruntime', is_system: true)
          break
        endif
      endif
    endforeach

    if ai_enabled_flag == '-DENABLE_AI=0' and enable_ai_opt.enabled()
      error('ONNX Runtime requested (enable_ai=enabled) but not found.')
    endif
  endif
endif

streamer_core_sources = [
  'src/common/config.cpp',
  'src/capture/source_capture.cpp',
  'src/pipeline/profiler.cpp',
  'src/pipeline/stats_writer.cpp',
  'src/inference/yolo_postprocess.cpp',
  'src/inference/yuv_overlay.cpp',
  'src/inference/ai_worker.cpp',
  'src/muxer/playback_manager.cpp',
  'src/muxer/hls_muxer.cpp'
]

streamer_core = static_library('streamer_core',
  streamer_core_sources,
  dependencies: [threads_dep] + libav_deps + onnx_deps,
  include_directories: inc_dirs + onnx_inc,
  cpp_args: [ai_enabled_flag]
)

executable('live_streamer',
  'src/main.cpp',
  link_with: streamer_core,
  dependencies: [threads_dep] + libav_deps + onnx_deps,
  include_directories: inc_dirs + onnx_inc,
  cpp_args: [ai_enabled_flag],
  install: true
)

if get_option('enable_tests')
  unit_tests = executable('unit_tests',
    'tests/unit_tests.cpp',
    link_with: streamer_core,
    dependencies: [threads_dep] + libav_deps + onnx_deps,
  include_directories: inc_dirs + onnx_inc,
    cpp_args: [ai_enabled_flag]
  )
  test('unit_tests', unit_tests)
endif
