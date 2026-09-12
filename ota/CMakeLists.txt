add_library(ridgeline_ota STATIC src/ota_manifest.cc)
target_include_directories(ridgeline_ota PUBLIC include)
target_link_libraries(ridgeline_ota PUBLIC ridgeline_core PRIVATE OpenSSL::Crypto ridgeline_warnings)
