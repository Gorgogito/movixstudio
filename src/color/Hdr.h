#pragma once

#include "color/ColorMath.h"
#include "core/Project.h"

// Conversión de señales SDR/HDR entre la de cada fuente y la de trabajo del proyecto. Es la
// referencia en la CPU de lo que hacen los shaders (layer.frag y canvas.frag).
//  - SDR: Rec.709 con gamma 2,4 (BT.1886); su blanco se coloca a 203 nits en HDR (BT.2408).
//  - PQ: SMPTE ST 2084 sobre Rec.2020, valores absolutos hasta 10 000 nits.
//  - HLG: ARIB STD-B67 sobre Rec.2020, con la OOTF de una pantalla de 1000 nits (gamma 1,2).
// De HDR a SDR se mapean los tonos sobre la luminancia: lineal hasta el 70 % del blanco SDR y,
// por encima, una compresión que lleva los 1000 nits al máximo de SDR.
namespace movix::color {

enum class Signal { SDR, PQ, HLG };

inline constexpr double kSdrWhiteNits = 203.0;
inline constexpr double kHdrPeakNits = 1000.0;
inline constexpr double kToneKnee = 0.7; // en unidades del blanco SDR

Signal signalOf(core::ColorMode mode);
// Convierte un color de la señal `from` (primarias Rec.2020 si `from2020`) a la del modo `to`.
Rgb convertSignal(Rgb in, Signal from, bool from2020, core::ColorMode to);
// ¿Hace falta convertir? (SDR Rec.709 en un proyecto SDR, o la misma señal HDR, no.)
bool needsConversion(Signal from, bool from2020, core::ColorMode to);

// Piezas sueltas (para pruebas).
double pqToNits(double e);
double toneMap(double y); // luminancia relativa al blanco SDR → SDR (0..1)
double nitsToPq(double nits);

} // namespace movix::color
