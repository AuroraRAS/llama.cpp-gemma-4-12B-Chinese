#include "llama.h"
#include "common.h"
#include "unicode.h"
#include "sampling.h"
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstring>

// Helper to determine if a string starts with standard ASCII spaces or SentencePiece/BPE prefix spaces
static bool starts_with_space(const std::string & str) {
    if (str.empty()) {
        return false;
    }
    if (str[0] == ' ') {
        return true;
    }
    if (str.size() >= 3 && str.compare(0, 3, "\xe2\x96\x81") == 0) {
        return true;
    }
    if (str.size() >= 2 && str.compare(0, 2, "\xc4\xa0") == 0) {
        return true;
    }
    if (str.size() >= 2 && str.compare(0, 2, "\xc5\xa0") == 0) {
        return true;
    }
    return false;
}

// Helper to strip ALL contiguous occurrences of space prefixes from the beginning of the string
static std::string strip_leading_spaces(const std::string & str) {
    std::string s = str;
    while (!s.empty()) {
        if (s[0] == ' ') {
            s = s.substr(1);
        } else if (s.size() >= 3 && s.compare(0, 3, "\xe2\x96\x81") == 0) {
            s = s.substr(3);
        } else if (s.size() >= 2 && s.compare(0, 2, "\xc4\xa0") == 0) {
            s = s.substr(2);
        } else if (s.size() >= 2 && s.compare(0, 2, "\xc5\xa0") == 0) {
            s = s.substr(2);
        } else {
            break;
        }
    }
    return s;
}

// Strict CJK check: returns true ONLY if EVERY non-space character in the string
// falls into CJK Unified Ideographs, CJK Symbols/Punctuation, or Fullwidth Forms.
// Handles invalid UTF-8 sequences gracefully.
static bool is_cjk_or_punctuation(const std::string & str) {
    if (str.empty()) {
        return false;
    }
    size_t offset = 0;
    bool has_cjk = false;
    while (offset < str.size()) {
        auto res = common_parse_utf8_codepoint(str, offset);
        if (res.status != utf8_parse_result::SUCCESS) {
            return false; // Handle invalid UTF-8 gracefully
        }
        uint32_t cp = res.codepoint;
        // Space characters to bypass (ASCII space, SentencePiece space, BPE space characters)
        bool is_space = (cp == ' ' || cp == 0x2581 || cp == 0x0120 || cp == 0x0160);
        if (!is_space) {
            bool is_cjk = (cp >= 0x4E00 && cp <= 0x9FFF) ||
                          (cp >= 0x3000 && cp <= 0x303F) ||
                          (cp >= 0xFF00 && cp <= 0xFFEF);
            if (!is_cjk) {
                return false; // Found a non-space character that is not CJK/punctuation
            }
            has_cjk = true;
        }
        offset += res.bytes_consumed;
    }
    return has_cjk;
}

// Helper function to map vocab attributes to token types as requested
static llama_token_type llama_vocab_get_type(const struct llama_vocab * vocab, llama_token token) {
    llama_token_attr attr = llama_vocab_get_attr(vocab, token);
    if (attr & LLAMA_TOKEN_ATTR_CONTROL) {
        return LLAMA_TOKEN_TYPE_CONTROL;
    }
    if (attr & LLAMA_TOKEN_ATTR_UNKNOWN) {
        return LLAMA_TOKEN_TYPE_UNKNOWN;
    }
    if (attr & LLAMA_TOKEN_ATTR_BYTE) {
        return LLAMA_TOKEN_TYPE_BYTE;
    }
    if (attr & LLAMA_TOKEN_ATTR_UNUSED) {
        return LLAMA_TOKEN_TYPE_UNUSED;
    }
    if (attr & LLAMA_TOKEN_ATTR_USER_DEFINED) {
        return LLAMA_TOKEN_TYPE_USER_DEFINED;
    }
    if (attr & LLAMA_TOKEN_ATTR_NORMAL) {
        return LLAMA_TOKEN_TYPE_NORMAL;
    }
    return LLAMA_TOKEN_TYPE_UNDEFINED;
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model_path> [output_path]\n", argv[0]);
        return 1;
    }

    std::string model_path = argv[1];
    std::string output_path;

    if (argc > 2) {
        output_path = argv[2];
    } else {
        // Dynamically derive output path from input path
        size_t last_slash = model_path.find_last_of("/\\");
        std::string filename = (last_slash == std::string::npos) ? model_path : model_path.substr(last_slash + 1);
        size_t last_dot = filename.find_last_of('.');
        std::string basename = (last_dot == std::string::npos) ? filename : filename.substr(0, last_dot);
        output_path = basename + "_cjk_strip.bin";
    }

    printf("CJK Strip Builder: loading model from '%s'...\n", model_path.c_str());

    // Initialize llama backend
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.vocab_only = true; // Load only the vocabulary, no weights

    llama_model * model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model) {
        fprintf(stderr, "error: failed to load model from '%s'\n", model_path.c_str());
        llama_backend_free();
        return 1;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);
    if (!vocab) {
        fprintf(stderr, "error: failed to retrieve vocabulary from model\n");
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    int32_t n_vocab = llama_vocab_n_tokens(vocab);
    printf("Successfully loaded vocabulary with %d tokens.\n", n_vocab);

    std::vector<std::vector<llama_token>> strip_map(n_vocab);
    std::vector<uint8_t> is_cjk_punct_cache(n_vocab, 0);
    std::vector<uint8_t> is_pure_space_cache(n_vocab, 0);
    int32_t mapped_count = 0;
    int32_t cjk_punct_count = 0;

    for (llama_token id = 0; id < n_vocab; ++id) {
        // Regardless of prefix spaces, populate state cache:
        std::string piece = common_token_to_piece(vocab, id, true);
        if (is_cjk_or_punctuation(piece)) {
            is_cjk_punct_cache[id] = 1;
            cjk_punct_count++;
        }

        if (piece == " " || piece == "\xe2\x96\x81" || piece == "\xc4\xa0" || piece == "\xc5\xa0") {
            is_pure_space_cache[id] = 1;
        }

        // Bypass special tokens for the mapping
        llama_token_type ttype = llama_vocab_get_type(vocab, id);
        if (ttype == LLAMA_TOKEN_TYPE_CONTROL || ttype == LLAMA_TOKEN_TYPE_UNKNOWN || ttype == LLAMA_TOKEN_TYPE_BYTE) {
            continue;
        }

        if (starts_with_space(piece)) {
            std::string pure_str = strip_leading_spaces(piece);
            if (is_cjk_or_punctuation(pure_str)) {
                // Tokenize the stripped string
                auto pure_tokens = common_tokenize(vocab, pure_str, false, false);
                if (!pure_tokens.empty()) {
                    std::string verify_str = "";
                    for (llama_token t : pure_tokens) {
                        verify_str += common_token_to_piece(vocab, t, true);
                    }
                    if (verify_str == pure_str) {
                        strip_map[id] = pure_tokens;
                        mapped_count++;
                        // Print mapping log
                        printf("Mapped: %6d ('%s') -> %zu tokens ('%s')\n", id, piece.c_str(), pure_tokens.size(), pure_str.c_str());
                    } else {
                        // Semantic collision detected
                        printf("Collision warning: ID %d ('%s') stripped to '%s', re-tokenized to %zu tokens starting with ID %d ('%s') [REJECTED]\n",
                               id, piece.c_str(), pure_str.c_str(), pure_tokens.size(), pure_tokens[0], verify_str.c_str());
                    }
                }
            }
        }
    }

    printf("Total CJK mappings identified: %d\n", mapped_count);
    printf("Total CJK punctuation characters identified: %d\n", cjk_punct_count);
    printf("Writing binary strip map to '%s'...\n", output_path.c_str());

    FILE * f = fopen(output_path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "error: failed to open output file '%s' for writing\n", output_path.c_str());
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // Header
    uint32_t magic = 0x53545250; // ASCII "STRP"
    uint32_t size = (uint32_t)n_vocab;

    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&size, sizeof(size), 1, f) != 1) {
        fprintf(stderr, "error: failed to write header to '%s'\n", output_path.c_str());
        fclose(f);
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // Payload: is_cjk_punct_cache
    if (fwrite(is_cjk_punct_cache.data(), sizeof(uint8_t), n_vocab, f) != (size_t)n_vocab) {
        fprintf(stderr, "error: failed to write punct cache payload to '%s'\n", output_path.c_str());
        fclose(f);
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // Payload: is_pure_space_cache
    if (fwrite(is_pure_space_cache.data(), sizeof(uint8_t), n_vocab, f) != (size_t)n_vocab) {
        fprintf(stderr, "error: failed to write pure space cache payload to '%s'\n", output_path.c_str());
        fclose(f);
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // Payload: strip_map (1-to-N mappings)
    for (int32_t id = 0; id < n_vocab; ++id) {
        uint8_t count = (uint8_t)strip_map[id].size();
        if (fwrite(&count, sizeof(count), 1, f) != 1) {
            fprintf(stderr, "error: failed to write mapping count for token %d to '%s'\n", id, output_path.c_str());
            fclose(f);
            llama_model_free(model);
            llama_backend_free();
            return 1;
        }
        if (count > 0) {
            if (fwrite(strip_map[id].data(), sizeof(llama_token), count, f) != count) {
                fprintf(stderr, "error: failed to write mapping tokens for token %d to '%s'\n", id, output_path.c_str());
                fclose(f);
                llama_model_free(model);
                llama_backend_free();
                return 1;
            }
        }
    }

    fclose(f);
    printf("Successfully wrote strip map binary.\n");

    // Test common_params_sampling initialization with the generated binary file
    printf("Testing sampler initialization with the generated binary map...\n");
    common_params_sampling sparams;
    {
        std::ifstream test_f(output_path, std::ios::binary);
        if (!test_f) {
            fprintf(stderr, "error: failed to open CJK strip map file for testing: %s\n", output_path.c_str());
            llama_model_free(model);
            llama_backend_free();
            return 1;
        }
        uint32_t magic_val;
        test_f.read(reinterpret_cast<char*>(&magic_val), sizeof(magic_val));
        uint32_t size_val;
        test_f.read(reinterpret_cast<char*>(&size_val), sizeof(size_val));
        
        sparams.cjk_punct_cache.resize(size_val);
        test_f.read(reinterpret_cast<char*>(sparams.cjk_punct_cache.data()), size_val * sizeof(uint8_t));

        sparams.is_pure_space_cache.resize(size_val);
        test_f.read(reinterpret_cast<char*>(sparams.is_pure_space_cache.data()), size_val * sizeof(uint8_t));

        sparams.cjk_strip_map.resize(size_val);
        for (uint32_t id = 0; id < size_val; ++id) {
            uint8_t count = 0;
            test_f.read(reinterpret_cast<char*>(&count), sizeof(count));
            if (count > 0) {
                sparams.cjk_strip_map[id].resize(count);
                test_f.read(reinterpret_cast<char*>(sparams.cjk_strip_map[id].data()), count * sizeof(llama_token));
            }
        }
    }

    // Try initializing a common_sampler
    struct common_sampler * gsmpl = common_sampler_init(model, sparams);
    if (!gsmpl) {
        fprintf(stderr, "error: failed to initialize common_sampler with CJK strip map\n");
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }
    printf("Successfully initialized common_sampler with CJK strip map!\n");
    common_sampler_free(gsmpl);

    // Cleanup resources
    llama_model_free(model);
    llama_backend_free();

    return 0;
}
