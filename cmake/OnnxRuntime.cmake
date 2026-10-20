# ONNX Runtime (MIT) precompilado: el paquete NuGet oficial de CPU, descargado y verificado con
# SHA-256 al configurar. Define el objetivo importado onnxruntime::onnxruntime y la variable
# ONNXRUNTIME_RUNTIME_DLLS (DLL que deben acompañar al ejecutable).
include(FetchContent)

set(MOVIX_ONNXRUNTIME_VERSION 1.31.0)
FetchContent_Declare(onnxruntime_pkg
    URL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${MOVIX_ONNXRUNTIME_VERSION}/microsoft.ml.onnxruntime.${MOVIX_ONNXRUNTIME_VERSION}.nupkg"
    URL_HASH SHA256=7e35fdb4f5adb62f01e60798e2e5ed05108f5fbbe1cca03fbcc37903cf7c5e3b
    DOWNLOAD_NAME onnxruntime.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(onnxruntime_pkg)

set(_ort "${onnxruntime_pkg_SOURCE_DIR}")
add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_ort}/runtimes/win-x64/native/onnxruntime.dll"
    IMPORTED_IMPLIB "${_ort}/runtimes/win-x64/native/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_ort}/build/native/include")
set(ONNXRUNTIME_RUNTIME_DLLS
    "${_ort}/runtimes/win-x64/native/onnxruntime.dll"
    "${_ort}/runtimes/win-x64/native/onnxruntime_providers_shared.dll")
set(ONNXRUNTIME_LICENSE_FILES "${_ort}/LICENSE" "${_ort}/ThirdPartyNotices.txt")

# Proveedor de ejecución WebGPU (plugin oficial, MIT; Dawn es BSD) para usar la GPU. Solo se copia
# su DLL: Dawn trabaja sobre Vulkan, así que no hacen falta dxcompiler.dll ni dxil.dll (este último
# tiene licencia propietaria de Microsoft).
FetchContent_Declare(onnxruntime_webgpu_pkg
    URL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.ep.webgpu/0.4.0/microsoft.ml.onnxruntime.ep.webgpu.0.4.0.nupkg"
    URL_HASH SHA256=cc2e093a2eaa760a630d6c3299f015f1dd86885b9eb5877257135308f7c5e923
    DOWNLOAD_NAME onnxruntime-webgpu.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(onnxruntime_webgpu_pkg)
list(APPEND ONNXRUNTIME_RUNTIME_DLLS "${onnxruntime_webgpu_pkg_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime_providers_webgpu.dll")
set(ONNXRUNTIME_WEBGPU_LICENSE_FILES "${onnxruntime_webgpu_pkg_SOURCE_DIR}/LICENSE" "${onnxruntime_webgpu_pkg_SOURCE_DIR}/ThirdPartyNotices.txt")
