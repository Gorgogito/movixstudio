# Descarga (una vez) los modelos de IA que usan las pruebas, verificados con SHA-256.
# Sin red no es un error: las pruebas que los necesitan se omiten.
#   cmake -DOUT_DIR=<carpeta> -P DownloadTestModels.cmake
set(MODELS
    "ggml-base.bin|https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin|60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"
    "ggml-silero-v6.2.0.bin|https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin|2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987"
    "rnnoise-somnolent-hogwash.rnnn|https://raw.githubusercontent.com/GregorR/rnnoise-models/master/somnolent-hogwash-2018-09-01/sh.rnnn|70bb6685eb0c2a1d18e2918dca3fbfbd39317010b1802eb1b6ea73a92f3fdec0"
    # Kokoro (texto a voz): modelo cuantizado, vocabulario y una voz en español.
    "kokoro-82m/model.onnx|https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231/onnx/model_quantized.onnx|fbae9257e1e05ffc727e951ef9b9c98418e6d79f1c9b6b13bd59f5c9028a1478"
    "kokoro-82m/config.json|https://huggingface.co/hexgrad/Kokoro-82M/resolve/f3ff3571791e39611d31c381e3a41a3af07b4987/config.json|5abb01e2403b072bf03d04fde160443e209d7a0dad49a423be15196b9b43c17f"
    "kokoro-82m/ef_dora.bin|https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231/voices/ef_dora.bin|f66ec66bd295acb18372e37008533a9a3228483ccd294e7538d5d9294ac9a532"
    # Opus-MT español → inglés e inglés → español.
    "opus-mt-es-en/encoder.onnx|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/encoder_model_quantized.onnx|c01e70f8455efc350831aa2af7ed187b16829241f0018ba07d1ba643a391bc18"
    "opus-mt-es-en/decoder.onnx|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/decoder_model_quantized.onnx|d518f1f0041b2831f0aeb5a087cf899742cd9d4b574f6b98ad9f816b8c06925b"
    "opus-mt-es-en/source.spm|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/source.spm|e236ee6d866b635c0142114f8647f39831f9d92534aa2aad75c942f6a78ad0e3"
    "opus-mt-es-en/target.spm|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/target.spm|4dd547c24816a335e7b0b2e63376a8f1b3cbfc671eda5ab808dd44fdadaa8791"
    "opus-mt-es-en/vocab.json|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/vocab.json|b074b4cca0036ade5a39ea97faabd534e1015482c480fc2cb02c6481983eb163"
    "opus-mt-en-es/encoder.onnx|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/encoder_model_quantized.onnx|03434edd1147e6c49e22066e9dc5888235e99cc35a6711d1c4ea5b06455d5da9"
    "opus-mt-en-es/decoder.onnx|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/decoder_model_quantized.onnx|2b45074cc819f1d901e40510d7bc0781d99327d16c7eca04518c4ee0c3d6ba10"
    "opus-mt-en-es/source.spm|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/source.spm|4dd547c24816a335e7b0b2e63376a8f1b3cbfc671eda5ab808dd44fdadaa8791"
    "opus-mt-en-es/target.spm|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/target.spm|e236ee6d866b635c0142114f8647f39831f9d92534aa2aad75c942f6a78ad0e3"
    "opus-mt-en-es/vocab.json|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/vocab.json|b074b4cca0036ade5a39ea97faabd534e1015482c480fc2cb02c6481983eb163"
    # HTDemucs (separación de voz y música).
    "htdemucs/htdemucs.onnx|https://huggingface.co/smank/htdemucs-onnx/resolve/fb1fd25aa6260c253d86bbb4aa26b73aebc79f10/htdemucs.onnx|d2b401f322558cd57d67a752ed7be3fa55178a0626011eda8ac7bb74e17280c0"
)
file(MAKE_DIRECTORY "${OUT_DIR}")
foreach(entry IN LISTS MODELS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    list(GET parts 2 sha)
    set(dest "${OUT_DIR}/${name}")
    get_filename_component(dest_dir "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${dest_dir}")
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" have)
        if(have STREQUAL sha)
            continue()
        endif()
    endif()
    message(STATUS "Descargando ${name}...")
    file(DOWNLOAD "${url}" "${dest}.part" EXPECTED_HASH SHA256=${sha} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(code EQUAL 0)
        file(RENAME "${dest}.part" "${dest}")
    else()
        file(REMOVE "${dest}.part")
        message(WARNING "No se pudo descargar ${name}: ${status}")
    endif()
endforeach()
