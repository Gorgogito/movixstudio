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
    "kokoro-82m/af_heart.bin|https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231/voices/af_heart.bin|d583ccff3cdca2f7fae535cb998ac07e9fcb90f09737b9a41fa2734ec44a8f0b"
    # Léxico inglés de misaki (el fonetizador de Kokoro).
    "kokoro-82m/us_gold.json|https://raw.githubusercontent.com/hexgrad/misaki/fba1236595f2d2bf21d414ba6e57d25256afada3/misaki/data/us_gold.json|dc414872a49a28ae6c141463d502fd945f3b2fde040484fdc47d00cc4612686f"
    "kokoro-82m/us_silver.json|https://raw.githubusercontent.com/hexgrad/misaki/fba1236595f2d2bf21d414ba6e57d25256afada3/misaki/data/us_silver.json|de8f67be911bb6c659187b4a65fd966b6a30e56350e0f790d763210b053ac475"
    # Opus-MT español → inglés e inglés → español.
    "opus-mt-es-en/encoder.onnx|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/encoder_model_q4f16.onnx|e0121c9448bbb91dce4e34c8e8f5eb64bcd14e4478bc67ead3c079a92ff2916a"
    "opus-mt-es-en/decoder.onnx|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/decoder_model_q4f16.onnx|9f581b5beb212f59027d427bb3c634a270349ec3985a9e52e9735e3ad1c30775"
    "opus-mt-es-en/source.spm|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/source.spm|e236ee6d866b635c0142114f8647f39831f9d92534aa2aad75c942f6a78ad0e3"
    "opus-mt-es-en/target.spm|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/target.spm|4dd547c24816a335e7b0b2e63376a8f1b3cbfc671eda5ab808dd44fdadaa8791"
    "opus-mt-es-en/vocab.json|https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/vocab.json|b074b4cca0036ade5a39ea97faabd534e1015482c480fc2cb02c6481983eb163"
    "opus-mt-en-es/encoder.onnx|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/encoder_model_q4f16.onnx|2d41f51f690a3dc9d1b4ae36b570dc801256cf4249290042bc375a75b0d424ca"
    "opus-mt-en-es/decoder.onnx|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/decoder_model_q4f16.onnx|b3cc431f1aa3265f80570e8c29d9dd1aa508490d5d9eb8100e0d54d81adedcfc"
    "opus-mt-en-es/source.spm|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/source.spm|4dd547c24816a335e7b0b2e63376a8f1b3cbfc671eda5ab808dd44fdadaa8791"
    "opus-mt-en-es/target.spm|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/target.spm|e236ee6d866b635c0142114f8647f39831f9d92534aa2aad75c942f6a78ad0e3"
    "opus-mt-en-es/vocab.json|https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/vocab.json|b074b4cca0036ade5a39ea97faabd534e1015482c480fc2cb02c6481983eb163"
    # HTDemucs (separación de voz y música).
    # IA visual (F5).
    "modnet/model.onnx|https://huggingface.co/Xenova/modnet/resolve/fa2fa546052fba4c08921230a26cc69a333fca12/onnx/model.onnx|07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9"
    "birefnet-lite/model.onnx|https://huggingface.co/onnx-community/BiRefNet_lite-ONNX/resolve/de15b22ba131738a16dff04aab8bdf8dc32e3ac1/onnx/model_fp16.onnx|d39b897ceb16ae654c1731f3dba0cf9b368d9cae74b5a57459b455cc8bfec402"
    "realesrgan-x4v3/model.onnx|https://huggingface.co/skillsafe-ai/realesr-general-x4v3/resolve/042a40bc4c918349ad3e2e607a68ae509a4c27b5/model.onnx|a946f7a9397021b9b6b7e71df3d2821b04cc09ff244423b7ca79cb191ce4a00e"
    "rife-4/model.onnx|https://huggingface.co/walterlow/RIFE_fp32_timestep/resolve/ee09066f9822f8b28b8477a1b4cc30f19d607590/RIFE_fp32_timestep.onnx|4da60c1f20d42dba4f21503140940aa48a488585ead977532bf22e95a0319327"
    "sam2-tiny/vision_encoder.onnx|https://huggingface.co/onnx-community/sam2.1-hiera-tiny-ONNX/resolve/814a066640debee5a91e70aa401fb8e17e030503/onnx/vision_encoder.onnx|4f30aacd3aaefbca81a0b7fe4c1fc96345570ea0a6f80ced599493d1b3be2e8c"
    "sam2-tiny/vision_encoder.onnx_data|https://huggingface.co/onnx-community/sam2.1-hiera-tiny-ONNX/resolve/814a066640debee5a91e70aa401fb8e17e030503/onnx/vision_encoder.onnx_data|e83df9866a5afe68ea7f0f721f18f65137fc3acbf0da1c74e946d363e09c69cc"
    "sam2-tiny/prompt_encoder_mask_decoder.onnx|https://huggingface.co/onnx-community/sam2.1-hiera-tiny-ONNX/resolve/814a066640debee5a91e70aa401fb8e17e030503/onnx/prompt_encoder_mask_decoder.onnx|874414704c5d686db7d206a35f6e15d26563d50c8c4468fccc6739bd7e491dcf"
    "sam2-tiny/prompt_encoder_mask_decoder.onnx_data|https://huggingface.co/onnx-community/sam2.1-hiera-tiny-ONNX/resolve/814a066640debee5a91e70aa401fb8e17e030503/onnx/prompt_encoder_mask_decoder.onnx_data|e9874d900dd4134ed60eab1e97910327c2419e0b2954485d8fd6e7f1a1470f47"
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
