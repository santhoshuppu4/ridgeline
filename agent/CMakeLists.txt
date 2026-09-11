add_executable(ridgeline_agent src/main.cc)
target_compile_definitions(ridgeline_agent PRIVATE RIDGELINE_VERSION="${PROJECT_VERSION}")
target_link_libraries(ridgeline_agent PRIVATE ridgeline_core ridgeline_proto ridgeline_warnings Threads::Threads)

# --video real-pipeline mode is only available when Phase 1b-ii's inference
# stack was built. Without it, the agent still builds and runs in
# fake-detection mode (used by scripts/smoke_test.sh), which needs neither
# ONNX Runtime nor OpenCV -- this mirrors RIDGELINE_WITH_ONNX gating the
# same way it does in the top-level CMakeLists.txt.
if(RIDGELINE_WITH_ONNX)
  target_link_libraries(ridgeline_agent PRIVATE ridgeline_onnx)
  target_compile_definitions(ridgeline_agent PRIVATE RIDGELINE_HAVE_ONNX=1
    RIDGELINE_DEFAULT_MODEL="${RIDGELINE_MODEL_PATH}")
endif()
