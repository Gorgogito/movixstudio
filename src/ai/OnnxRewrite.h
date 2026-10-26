#pragma once

#include <cstddef>
#include <string>

// Reescritura de un modelo ONNX para que entero quepa en la GPU (WebGPU), sin depender de la
// biblioteca de ONNX: se lee el protobuf directamente y todo lo que no cambia se copia byte a byte.
//  - Split con más salidas de las que admite un sombreador (entrada + salidas > `maxBuffers`) y
//    tamaños constantes → un Slice por salida.
//  - Sum (que el proveedor WebGPU no tiene) → cadena de Add.
//  - ConvTranspose 1D de núcleo grande (una STFT inversa, por ejemplo), muy lento en WebGPU →
//    MatMul y suma con solapamiento.
// Así esos nodos no van a la CPU, lo que obliga a bajar y volver a subir los tensores en cada uno.
namespace movix::ai::onnx {

struct RewriteStats {
    int splits = 0;         // Split convertidos en Slice
    int sums = 0;           // Sum convertidos en Add
    int convTransposes = 0; // ConvTranspose convertidos en MatMul
};

// Devuelve el modelo reescrito, o una cadena vacía si no hay nada que cambiar o el archivo usa algo
// que esta lectura no entiende (pesos en archivos externos, por ejemplo): entonces se carga tal cual.
std::string rewriteForGpu(const char* data, size_t size, int maxBuffers, RewriteStats* stats = nullptr);

} // namespace movix::ai::onnx
