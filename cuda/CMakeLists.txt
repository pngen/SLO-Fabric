# SLO Fabric CUDA proof. Enabled only when SLOFABRIC_BUILD_CUDA=ON.
enable_language(CUDA)

add_executable(slofabric_cuda_proof cuda_proof.cu)
target_link_libraries(slofabric_cuda_proof PRIVATE slofabric)
target_include_directories(slofabric_cuda_proof PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set_target_properties(slofabric_cuda_proof PROPERTIES CUDA_ARCHITECTURES "120")
target_compile_options(slofabric_cuda_proof PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W3,/EHsc,/utf-8>)
