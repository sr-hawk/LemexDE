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

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// A token streamed during generation.
typedef void (^LLMTokenHandler)(NSString *token);
/// Polled between tokens; return YES to stop generation cooperatively.
typedef BOOL (^LLMCancelCheck)(void);

/// How a generation ended (mirrors Swift FinishReason).
typedef NS_ENUM(int32_t, LLMFinishReason) {
    LLMFinishReasonStop = 0,
    LLMFinishReasonLength = 1,
    LLMFinishReasonCancelled = 2,
};

/// Decoding parameters for one generation.
@interface LLMSamplingConfig : NSObject
@property (nonatomic) float temperature;   // <= 0 selects greedy decoding
@property (nonatomic) float topP;
@property (nonatomic) int32_t maxTokens;
@property (nonatomic) uint32_t seed;
@property (nonatomic, copy, nullable) NSString *grammar; // optional GBNF
@end

/// Outcome of one generation.
@interface LLMGenerationResult : NSObject
@property (nonatomic, copy) NSString *text;
@property (nonatomic) LLMFinishReason finishReason;
@property (nonatomic) int32_t promptTokens;
@property (nonatomic) int32_t completionTokens;
@end

/// Thin Objective-C++ wrapper over llama.cpp. NOT thread-safe: a llama context
/// must be driven from one thread at a time — callers (LlamaCppBackend) serialize
/// access and run generation off the main thread.
@interface LLMEngine : NSObject

@property (nonatomic, readonly) BOOL isLoaded;
@property (nonatomic, readonly, copy) NSString *modelDescription;
/// Context size the model was loaded with.
@property (nonatomic, readonly) uint32_t contextLength;

/// Load a GGUF model. `nGpuLayers` layers are offloaded to Metal (pass a large
/// value to offload everything). Returns NO and fills `error` on failure.
- (BOOL)loadModelAtPath:(NSString *)path
                   nCtx:(uint32_t)nCtx
             nGpuLayers:(int32_t)nGpuLayers
                  error:(NSError **)error;

- (void)unload;

/// Render messages into a prompt using the model's built-in chat template.
/// `roles` and `contents` are parallel arrays. Returns nil if no template.
- (nullable NSString *)applyChatTemplateWithRoles:(NSArray<NSString *> *)roles
                                         contents:(NSArray<NSString *> *)contents
                                     addAssistant:(BOOL)addAssistant;

/// Number of tokens `text` would tokenize to (for the memory/UX surfaces).
- (int32_t)tokenCountForText:(NSString *)text;

/// Generate a completion for `prompt`, streaming tokens via `onToken` and
/// polling `isCancelled`. Returns nil and fills `error` on failure.
- (nullable LLMGenerationResult *)generateWithPrompt:(NSString *)prompt
                                            sampling:(LLMSamplingConfig *)sampling
                                             onToken:(nullable LLMTokenHandler)onToken
                                         isCancelled:(nullable LLMCancelCheck)isCancelled
                                               error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
