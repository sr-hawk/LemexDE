/*
 SPDX-License-Identifier: AGPL-3.0-or-later

 Copyright (C) 2026 emexlab

 This file is part of Nyxian.

 Nyxian is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Nyxian is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with Nyxian. If not, see <https://www.gnu.org/licenses/>.
*/

#import "LLMEngine.h"

#include "llama.h"

#include <string>
#include <vector>

NSString *const LLMEngineErrorDomain = @"org.emexlab.nyxian.LLMEngine";

@implementation LLMSamplingConfig
@end

@implementation LLMGenerationResult
@end

// MARK: - C++ helpers

static std::vector<llama_token> LLMTokenize(const llama_vocab *vocab, const std::string &text, bool addSpecial) {
    int32_t needed = -llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), NULL, 0, addSpecial, true);
    if (needed <= 0) {
        return {};
    }
    std::vector<llama_token> tokens(needed);
    int32_t got = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), tokens.data(), needed, addSpecial, true);
    if (got < 0) {
        return {};
    }
    tokens.resize(got);
    return tokens;
}

static std::string LLMPieceForToken(const llama_vocab *vocab, llama_token token) {
    char buf[256];
    int32_t n = llama_token_to_piece(vocab, token, buf, (int32_t)sizeof(buf), 0, /*special=*/false);
    if (n >= 0) {
        return std::string(buf, (size_t)n);
    }
    std::vector<char> big((size_t)(-n));
    int32_t m = llama_token_to_piece(vocab, token, big.data(), (int32_t)big.size(), 0, false);
    if (m < 0) {
        return std::string();
    }
    return std::string(big.data(), (size_t)m);
}

// MARK: - LLMEngine

@implementation LLMEngine {
    llama_model *_model;
    llama_context *_ctx;
    const llama_vocab *_vocab;
    uint32_t _nCtx;
    NSString *_modelDescription;
}

+ (void)ensureBackend {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        llama_backend_init();
    });
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _model = NULL;
        _ctx = NULL;
        _vocab = NULL;
        _nCtx = 0;
    }
    return self;
}

- (void)dealloc {
    [self unload];
}

- (BOOL)isLoaded {
    return _model != NULL && _ctx != NULL;
}

- (NSString *)modelDescription {
    return _modelDescription ?: @"(no model)";
}

- (uint32_t)contextLength {
    return _nCtx;
}

static NSError *LLMMakeError(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:LLMEngineErrorDomain
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: message }];
}

- (BOOL)loadModelAtPath:(NSString *)path
                   nCtx:(uint32_t)nCtx
             nGpuLayers:(int32_t)nGpuLayers
                  error:(NSError **)error {
    [LLMEngine ensureBackend];
    [self unload];

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = nGpuLayers;

    _model = llama_model_load_from_file(path.fileSystemRepresentation, mparams);
    if (!_model) {
        if (error) *error = LLMMakeError(1, [NSString stringWithFormat:@"Failed to load model at %@", path]);
        return NO;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = nCtx;

    _ctx = llama_init_from_model(_model, cparams);
    if (!_ctx) {
        llama_model_free(_model);
        _model = NULL;
        if (error) *error = LLMMakeError(2, @"Failed to create llama context");
        return NO;
    }

    _vocab = llama_model_get_vocab(_model);
    _nCtx = llama_n_ctx(_ctx);

    // Metal carries the heavy compute; leave a core for the rest of the app.
    int32_t threads = (int32_t)MAX(1, (NSInteger)[NSProcessInfo processInfo].activeProcessorCount - 1);
    llama_set_n_threads(_ctx, threads, threads);

    _modelDescription = path.lastPathComponent;
    return YES;
}

- (void)unload {
    if (_ctx) {
        llama_free(_ctx);
        _ctx = NULL;
    }
    if (_model) {
        llama_model_free(_model);
        _model = NULL;
    }
    _vocab = NULL;
    _nCtx = 0;
    _modelDescription = nil;
}

- (NSString *)applyChatTemplateWithRoles:(NSArray<NSString *> *)roles
                                contents:(NSArray<NSString *> *)contents
                            addAssistant:(BOOL)addAssistant {
    if (!_model || roles.count == 0 || roles.count != contents.count) {
        return nil;
    }
    const char *tmpl = llama_model_chat_template(_model, NULL);
    if (!tmpl) {
        return nil;
    }

    NSUInteger n = roles.count;
    // Keep the UTF-8 backing strings alive for the duration of the call; reserve
    // up front so the vector never reallocates and the c_str() pointers stay valid.
    std::vector<std::string> store;
    store.reserve(n * 2);
    for (NSUInteger i = 0; i < n; i++) {
        store.push_back(roles[i].UTF8String ?: "");
        store.push_back(contents[i].UTF8String ?: "");
    }
    std::vector<llama_chat_message> messages(n);
    for (NSUInteger i = 0; i < n; i++) {
        messages[i].role = store[i * 2].c_str();
        messages[i].content = store[i * 2 + 1].c_str();
    }

    int32_t needed = llama_chat_apply_template(tmpl, messages.data(), n, addAssistant, NULL, 0);
    if (needed <= 0) {
        return nil;
    }
    std::vector<char> buf((size_t)needed);
    int32_t written = llama_chat_apply_template(tmpl, messages.data(), n, addAssistant, buf.data(), needed);
    if (written <= 0) {
        return nil;
    }
    return [[NSString alloc] initWithBytes:buf.data() length:(NSUInteger)written encoding:NSUTF8StringEncoding];
}

- (int32_t)tokenCountForText:(NSString *)text {
    if (!_vocab) {
        return 0;
    }
    std::string t = text.UTF8String ?: "";
    return -llama_tokenize(_vocab, t.c_str(), (int32_t)t.size(), NULL, 0, true, true);
}

- (LLMGenerationResult *)generateWithPrompt:(NSString *)prompt
                                   sampling:(LLMSamplingConfig *)sampling
                                    onToken:(LLMTokenHandler)onToken
                                isCancelled:(LLMCancelCheck)isCancelled
                                      error:(NSError **)error {
    if (!_ctx || !_vocab) {
        if (error) *error = LLMMakeError(3, @"No model loaded");
        return nil;
    }

    // Reset the KV cache so each completion starts clean (single-shot per call).
    llama_memory_t mem = llama_get_memory(_ctx);
    if (mem) {
        llama_memory_clear(mem, true);
    }

    std::string promptStd = prompt.UTF8String ?: "";
    std::vector<llama_token> tokens = LLMTokenize(_vocab, promptStd, /*addSpecial=*/true);
    if (tokens.empty()) {
        if (error) *error = LLMMakeError(4, @"Tokenization failed");
        return nil;
    }

    int32_t promptCount = (int32_t)tokens.size();
    if ((uint32_t)promptCount >= _nCtx) {
        if (error) *error = LLMMakeError(5, @"Prompt longer than context window");
        return nil;
    }

    // Build the sampler chain. Grammar (if any) constrains logits before sampling.
    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (sampling.grammar.length > 0) {
        llama_sampler *grammar = llama_sampler_init_grammar(_vocab, sampling.grammar.UTF8String, "root");
        if (!grammar) {
            llama_sampler_free(smpl);
            if (error) *error = LLMMakeError(6, @"Invalid GBNF grammar");
            return nil;
        }
        llama_sampler_chain_add(smpl, grammar);
    }
    if (sampling.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(sampling.topP, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(sampling.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(sampling.seed));
    }

    NSMutableString *out = [NSMutableString string];
    LLMFinishReason finish = LLMFinishReasonStop;
    int32_t generated = 0;
    int32_t maxTokens = sampling.maxTokens > 0 ? sampling.maxTokens : INT32_MAX;
    int32_t nPast = promptCount;
    BOOL hardFailure = NO;

    // Decode the prompt. NOTE: assumes the prompt fits in one batch (n_batch);
    // long prompts should be chunked — fine for the Session A demo.
    llama_batch batch = llama_batch_get_one(tokens.data(), (int32_t)tokens.size());
    if (llama_decode(_ctx, batch) != 0) {
        llama_sampler_free(smpl);
        if (error) *error = LLMMakeError(7, @"Failed to decode prompt");
        return nil;
    }

    while (true) {
        if (isCancelled && isCancelled()) {
            finish = LLMFinishReasonCancelled;
            break;
        }

        llama_token tok = llama_sampler_sample(smpl, _ctx, -1);
        if (llama_vocab_is_eog(_vocab, tok)) {
            finish = LLMFinishReasonStop;
            break;
        }
        llama_sampler_accept(smpl, tok);

        std::string piece = LLMPieceForToken(_vocab, tok);
        if (!piece.empty()) {
            NSString *pieceStr = [[NSString alloc] initWithBytes:piece.data() length:piece.size() encoding:NSUTF8StringEncoding];
            if (pieceStr) {
                [out appendString:pieceStr];
                if (onToken) {
                    onToken(pieceStr);
                }
            }
        }

        generated++;
        if (generated >= maxTokens) {
            finish = LLMFinishReasonLength;
            break;
        }
        if ((uint32_t)(nPast + 1) >= _nCtx) {
            finish = LLMFinishReasonLength;
            break;
        }

        llama_batch next = llama_batch_get_one(&tok, 1);
        if (llama_decode(_ctx, next) != 0) {
            hardFailure = YES;
            break;
        }
        nPast++;
    }

    llama_sampler_free(smpl);

    if (hardFailure && out.length == 0) {
        if (error) *error = LLMMakeError(8, @"Decode failed during generation");
        return nil;
    }

    LLMGenerationResult *result = [LLMGenerationResult new];
    result.text = out;
    result.finishReason = finish;
    result.promptTokens = promptCount;
    result.completionTokens = generated;
    return result;
}

@end
