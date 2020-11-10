# Copyright (c) the JPEG XL Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(JPEGXL_EXTRAS_SOURCES
  extras/codec.cc
  extras/codec.h
  extras/codec_pgx.cc
  extras/codec_pgx.h
  extras/codec_png.cc
  extras/codec_png.h
  extras/codec_pnm.cc
  extras/codec_pnm.h
)

# We only define a static library for jxl_extras since it uses internal parts
# of jxl library which are not accessible from outside the library in the
# shared library case.
add_library(jxl_extras-static STATIC "${JPEGXL_EXTRAS_SOURCES}")
target_compile_options(jxl_extras-static PRIVATE "${JPEGXL_INTERNAL_FLAGS}")
set_property(TARGET jxl_extras-static PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(jxl_extras-static PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}"
)
target_link_libraries(jxl_extras-static PUBLIC
  jxl-static
  lodepng
)

find_package(GIF 5)
if(GIF_FOUND)
  target_sources(jxl_extras-static PRIVATE
    extras/codec_gif.cc
    extras/codec_gif.h
  )
  target_include_directories(jxl_extras-static PUBLIC "${GIF_INCLUDE_DIRS}")
  target_link_libraries(jxl_extras-static PUBLIC ${GIF_LIBRARIES})
  target_compile_definitions(jxl_extras-static PUBLIC -DJPEGXL_ENABLE_GIF=1)
endif()

find_package(JPEG)
if(JPEG_FOUND)
  target_sources(jxl_extras-static PRIVATE
    extras/codec_jpg.cc
    extras/codec_jpg.h
  )
  target_include_directories(jxl_extras-static PUBLIC "${JPEG_INCLUDE_DIRS}")
  target_link_libraries(jxl_extras-static PUBLIC ${JPEG_LIBRARIES})
  target_compile_definitions(jxl_extras-static PUBLIC -DJPEGXL_ENABLE_JPEG=1)
endif()

find_package(ZLIB)  # dependency of PNG
find_package(PNG)
if(PNG_FOUND AND ZLIB_FOUND)
  target_sources(jxl_extras-static PRIVATE
    extras/codec_apng.cc
    extras/codec_apng.h
  )
  target_include_directories(jxl_extras-static PUBLIC "${PNG_INCLUDE_DIRS}")
  target_link_libraries(jxl_extras-static PUBLIC ${PNG_LIBRARIES})
  target_compile_definitions(jxl_extras-static PUBLIC -DJPEGXL_ENABLE_APNG=1)
endif()

if (JPEGXL_ENABLE_SJPEG)
  target_compile_definitions(jxl_extras-static PUBLIC -DJPEGXL_ENABLE_SJPEG=1)
  target_link_libraries(jxl_extras-static PUBLIC sjpeg)
endif ()

if (JPEGXL_ENABLE_OPENEXR)
pkg_check_modules(OpenEXR IMPORTED_TARGET OpenEXR)
if (OpenEXR_FOUND)
  target_sources(jxl_extras-static PRIVATE
    extras/codec_exr.cc
    extras/codec_exr.h
  )
  target_compile_definitions(jxl_extras-static PUBLIC -DJPEGXL_ENABLE_EXR=1)
  target_link_libraries(jxl_extras-static PUBLIC PkgConfig::OpenEXR)
endif() # OpenEXR_FOUND
endif() # JPEGXL_ENABLE_OPENEXR