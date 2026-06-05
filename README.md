# CJK Token Strip Map & Sampling Hijacker (Tokenizer Parallax Fix)

This utility addresses the "Tokenizer Parallax" hallucination (unwanted prefix-space generation in CJK text) common in BPE-based models such as Gemma 4.

The solution consists of:
1. **Offline Builder (`llama-cjk-strip-builder`)**: Extracts CJK tokens with prefix spaces, performs strict bidirectional verification to avoid semantic collisions, and generates a unified binary map (`.bin`) containing the translation mapping and a CJK punctuation cache.
2. **Runtime Interceptor (`--cjk-strip-map`)**: Loads the map system-wide in `llama-cli`/`llama-server` and intercepts token sampling at the lowest sampler level using $O(1)$ lookups.
3. **Heuristic Self-Speculative Rollback**: Handles isolated "pure space" tokens at the application layer (e.g. `llama-completion`) using a sliding window. Upon detecting a `[CJK Punct] -> [Space] -> [CJK]` sequence, it mathematically rolls back the KV cache (`n_past -= 2`), re-decodes the rescued CJK character directly, erases the hallucinated space from the terminal using dynamic UTF-8 column width backspacing (`\b \b`), rewinds the sampler Markov chain (`common_sampler_pop`), and syncs all display/output buffers.


Before：

`llama.cpp/cbuild/bin/llama-cli --model ~/gguf/gemma-4-12B-it-Q4_K_M.gguf --system-prompt 'Always respond natively and fluently in Simplified Chinese.'`

```
> 你认识上海迪士尼乐园的玲娜贝儿吗？她的英文名是Linabell，是一只粉色的狐狸。

[Start thinking]
*   User asks: "Do you know Linabell from Shanghai Disneyland? Her English name is Linabell, and she is a pink fox."
    *   Key Information:
        *   Subject: Linabell (迪士尼乐园的 玲娜贝儿).
        *   Location: Shanghai Disneyland.
        *   Appearance: Pink fox.
    *   Language: Simplified Chinese (as requested by system instruction).

    *   Is Linabell a character in Shanghai Disneyland? Yes.
    *   What is her role? She is the mascot/character for the "Sparkle River" area (formerly known as "Adventure Town" or similar, now part't of the newer developments) and specifically associated with the "Disney Sparkle" aesthetic.
    *   What is her identity? She is a fox character.
    *   Appearance: Pink fur, very popular among fans, especially younger audiences.

    *   *Acknowledgment:* Confirm that I know her.
    *   *Description:* Briefly describe her characteristics (appearance, personality).
    *   *Context:* Mention her role in Shanghai Disneyland.
    *   *Cultural Note:* Mention her popularity (she's very popular for merch/meet-and-greets).

    *   *Initial thought:* 是的，我知道她。她是 迪士尼乐园 的 角色。 她 很 可爱。
    *   *Refining for fluency and "native" feel:*
        *   "是的" $\rightarrow$ "是的" is okay, but "是的，我知道" or "是的，我非常了解" sounds better.
        *   "她是迪士尼乐园的角色" $\rightarrow$ "她是上海迪士尼乐园的核心角色之一" (one of the key characters).
        *   "她 很有名气" $\rightarrow$ "她 在 粉丝 圈 很有 名气" (She is very popular in the fan circle).
    *   *Adding detail:* Mention her specific look (pink fox) and her role in the park.

    "是的，我了解。**玲娜贝儿 (Linabell)** 是上海迪士尼乐园非常受欢迎的一个角色。

    她确实是**一只 粉色 的 狐狸**，外貌 非常 可爱，特别是 那个 软萌 的 造型 让 她 成为 许多 粉丝、特别是 小朋友 们的 最爱。

     在 上海 迪士尼， 她 经常 出现在 各种 活动、游园 里的 互动 环节， 而且 还有很多 很有 名气 的 周边 产品。 粉丝 们 常常 会 去 找 她 互动 或者 是 拍 照片。

     你 是 她的 粉丝吗？或者 你 准备 去 上海 迪士尼 看 她 了吗？"

    *   Actually, the user's input has some slightly "non-native" spacing (e.g., "的", "是", "有", "的"). I should respond in natural, fluent Chinese without those spaces, but keep the content relevant.

    *   *Drafting final version:*
        是的，我非常了解。**玲娜贝儿 (Linabell)** 是上海迪士尼乐园非常受欢迎的核心角色之一。

        正如你 所说，她 是 一只 **粉色 的 狐狸**。 她 的 外貌 非常 萌系（kawaii），特别 是 那个 软萌、有 活力 的 设计， 让 她 成 为 很多 粉丝（特别是 小 朋友们） 的 最爱。

         在 上海 迪士尼， 她 不仅 出现 在 各种 互动 互动 的 场合， 还有 许多 专门 为 她 设计 的 周边 产品。 许多 粉丝 们 都 很 喜欢 去 找 她 互动 或 是 拍照。

         你 是 她的 粉丝吗？或者 你 是 准备 去 上海 迪士尼 寻找 她 了？

    *Wait, the prompt asks to respond "natively and fluently".* The user's input had spaces between characters (e.g., "是", "的"). I should respond in normal Chinese (no spaces) while keeping the tone friendly.

    *Revised Content:*
    是的，我非常了解！**玲娜裴儿 (Linabell)** 是上海迪士尼乐园非常受欢迎的核心角色之一。

    正如你 所 说， 她 是 一只**粉色 的 狐狸**。 她 的 外貌 非常 萌系， 特別 是 那个 软萌、有 活力 的 设计， 让 她 成 为 许多 粉丝（特别是 小 朋友们） 的 最爱。

    在 上海 迪士尼， 她 不仅 出现 在 各种 互动 的 场合， 还 有 许多 专门 为 她 设计 的 周 边 产 品。 很多 粉丝 们 都 很 喜欢 去 找 她 互动 或 是 拍 照。

    你 是 她 的 粉丝 吗？或者 你 是 准备 去 上海 迪士尼 寻找 她 了？

    *Wait, looking at the user's input again:* "你是 范 吗？" - No, the user didn't use spaces in the input, they just had some extra spaces in my mental scan? Let me re-read.
    User: "你 关系 上海 迪士尼 乐园 的 玲娜贝儿吗？ 她的 英文 名 是 Linabell， 是 一 圈 色的 狐狸。"
    Actually, the user's input *does* have spaces between most characters. This often happens with certain translation tools or input methods. I should reply in standard fluent Chinese (no spaces).

    *Final Polish:*
    是的，我非常了解！**玲娜贝儿 (Linibeer/Linabell)** 是上海迪士尼乐园非常受欢迎的一个角色。

    正如你 所 说， 她 是 一 圈 色的 狐 狸。 她 的 外 貌 非 常 可 爱， 特 别 是 那 个 软 萌、有 活 力 的 设 一 么， 让 她 成 为 很 多 芬 然（特别 是 小 朋 头 们） 的 最 爱。

    在 上海 迪士尼， 她 不 仅 出 現 在 各 種 互 动 的 场 合， 还 有 很 多 专 民 为 她 设 一 么 的 周 边 产 品。 很 多 芬 然 们 都 很 好 喜 合 去 找 她 互 动 或 是 拍 照。

    你 是 她 的 范 然 吗？好er 你 是 准 ប 去 上 海 迪士尼 寻找 她 了？

    *Self-Correction:* Wait, why am I adding spaces? I shouldn't. I should write naturally.

    *Final Plan:*
    - Confirm knowledge of Linabell.
    - Describe her as a cute pink fox.
    - Mention her popularity in Shanghai Disneyland.
    - Ask if the user is a fan.

    *Drafting the final response:*
    是的，我非常了解！**玲娜贝儿 (Linabell)** 是上海迪士尼乐园非常受欢迎的一个角色。

    正如你 所 说， 她 是 一 圈 色的 狐 狸。 她 的 外 貌 非 常 可 爱， 特 别 是 那 个 软 萌、有 活 力 的 设 一 么， 让 她 成 为 很 多 芬 然（特 别 是 小 朋 孩 们） 的 最 爱。

    在 上 海 迪士尼， 她 不 仅 出 現 在 各 種 互 动 的 场 合， 还 有 很 多 专 民 为 她 设 一 么 的 周 邊 產 品。 很 多 芬 然 們 都 很 好 喜 好 去 找 她 互 動 或 是 拍 照。

    你 是 她 的 范 然 嗎？好 麼 你 是 準 備 去 上 海 迪士尼 尋 找 她 了？

    Wait, I keep typing with spaces because I'm misreading the user's input. The user's input actually has spaces. Let me re-read carefully:
    "你 关系 上海 迪士尼 乐园 的 玲娜贝儿 吗？ 她的 英文 名 是 Linabell， 是 一 圈 色的 🦊 狸。"
    Okay, the user *did* use spaces. But the instructions say "Respond natively and fluently". This usually means standard Chinese without spaces.

    *Response Content:*
    "是的，我非常了解！**玲嬷贝儿 (Linabell)** 是上海迪士尼乐园非常受欢迎的一个角色。

    正如你 所 说， 她 是 一 只**粉色 的 狐 狸**。 她 的 外 貌 非 常 萌 系

[ Prompt: 26.2 t/s | Generation: 4.8 t/s ]

> /exit
```

After:

`llama.cpp/build/bin/llama-cli --model ~/gguf/gemma-4-12B-it-Q4_K_M.gguf --cjk-strip-map llama.cpp/gemma-4_cjk_strip.bin --system-prompt 'Always respond natively and fluently in Simplified Chinese.'`

```
> 你认识上海迪士尼乐园的玲娜贝儿吗？她的英文名是Linabell，是一只粉色的狐狸。

[Start thinking]
Shanghai Disneyland (上海迪士尼乐园).
Linabell (玲娜贝儿).
 A pink fox (粉色的狐狸).
 Simplified Chinese (Simplified Chinese).
 The user is asking if I know her and describing her.

    *   *What is Shanghai Disneyland?* It's the Disney theme park in Shanghai, China.
    *   *Who is Linabell?* She is a mascot specifically designed for the Shanghai park. She's a "fox" character, which is unique because Disney usually focuses on bears (Chip 'n' Dale), mice (Mickey/Minnie), or other animals.
    *   *Characteristics:* Pink fur, expressive eyes, very popular among younger fans and locals. She's part of the "Shanghai Disney" specific IP.

    *   *Direct Answer:* Yes, I know her.
    *   *Confirmation:* Acknowledge the description (pink fox, name Linabell).
    *   *Contextual details:* Mention her special status as a mascot unique to Shanghai Disneyland.
    *   *Personality/Role:* Mention her cute personality and popularity.
    *   *Cultural Context:* Mention why she's popular (unique design, fan favorite).

    *   *Initial thought:* 是的，我知道。她是上海迪士尼的角色。
    *   *Refining (More natural/fluent):* 当然是的！我知道玲娜贝儿（Linabell）。她是上海迪士尼乐园非常受欢迎的原创角色。

    *   *Adding details:*
        *   She's a pink fox (没错的粉色狐狸).
        *   She's a "local" icon (专门为上海设计的).
        *   She's very popular among kids and young people (受年轻人和 粉丝 喜爱).

    *   *Polishing the Chinese (Simplified):*
        "是的，我当然知道**玲娜贝儿 (Linabell)**！"
        "她是**上海迪士尼乐园 (Shanghai Disneyland)** 非常受欢迎的原创角色。"
        "正如你所说的，她是一位**粉色的狐狸**。她的设计非常可爱，也是 很多 粉丝的 核心 角色。"

    *   *Adding extra value:* Maybe mention her unique status in the Disney universe.

    "是的，我当然知道**玲娜贝儿 (Linabell)**！

    她是 **上海迪士尼乐园 (Shanghai Disneyland)** 专门 研发并 推出的原创 角色。 就像你所说的，她 是一只非常可爱的**粉色 狐狸**。

     相比于 传统的米奇或 其它 角色， 玲娜贝儿因为 独特的设计和 亲近心的性格，在上海 粉丝 圈里非常受欢迎。她的 形象被 认为是上海 迪士尼的 标志性 符号。

     你是 她的 粉丝吗？或者你有去过 迪士尼 见到她吗？"

    *Correction:* "他们" -> "她" (she). "他们" is for "them".
    *Correction:* "既然" -> "就像".
    *Correction:* "由于" -> "因为".

    *Final Version:*
    是的，我当然知道**玲娜贝儿 (Linabell)**！

    她是 **上海迪士尼乐园 (Shanghai Disneyland)** 专门 研发并 推出的原创 角色。 就像你所说的，她 是一只非常可爱的** 粉色 狐狸**。

     因为她 拥有 独特的设计和 亲近心的性格，她在上海 粉丝 圈里非常受欢迎， 甚至 成为了 很多 朋友和年轻人的心头 喜爱。 相比于 传统的 迪士尼 角色， 玲娜贝儿 算是了 专门 属于上海 迪士尼的 标志性 形象。

     你是 她的 粉丝吗？或者你有去过 迪士尼 亲近 或是 拍照 吗？
[End thinking]

是的，我当然知道**玲娜贝儿 (Linabell)**！

她是 **上海迪士尼乐园 (Shanghai Disneyland)** 专门 研发并 推出的原创 角色。 就像你所说的，她 是一只非常可爱的** 粉色 狐狸**。

 相比于 那些 知名度很高的 传统 迪士尼 角色（比如米奇或 蒂美儿）， 玲娜贝儿因为她 独特的设计、 亲近心的性格以及 漂亮的 形象，在上海 粉丝 圈里非常受欢迎， 甚至 成为了 很多 粉丝（尤其是年轻人和 孩子）的心头 喜爱。 

 她被 视为是 展现上海 迪士尼 独特 魅力的 标志性 形象。

 你是 她的 粉丝吗？或者你有去过 迪士尼 亲自 见过她吗？

[ Prompt: 25.5 t/s | Generation: 4.9 t/s ]

> /exit
```

---

## 1. Build the Offline Builder

The builder target is compiled automatically as part of the standard CMake configuration.

```bash
mkdir build && cd build
cmake ..
cmake --build . --target llama-cjk-strip-builder
```

---

## 2. Generate the Binary Map (Offline Step)

Run the builder on your target GGUF model to output the `.bin` mapping file.

Using `~/gguf/gemma-4-12B-it-Q4_K_M.gguf` as an example:

```bash
./bin/llama-cjk-strip-builder ~/gguf/gemma-4-12B-it-Q4_K_M.gguf gemma-4_cjk_strip.bin
```

### What happens under the hood?
1. The builder loads the model vocabulary (running in `vocab_only` mode for speed).
2. It identifies CJK tokens starting with spaces (both standard ASCII space and Unicode space characters like SentencePiece ` `).
3. It performs a **strict round-trip verification**:
   * It strips the space prefixes: `pure_str = strip_leading_spaces(piece)`.
   * It tokenizes the stripped piece: `pure_tokens = tokenize(pure_str)`.
   * It decodes the token: `verify_str = decode(pure_tokens[0])`.
   * It only registers the mapping if `pure_tokens.size() == 1` and `verify_str == pure_str`. This avoids BPE sub-word fallbacks or severe semantic collisions (e.g., mapping the stripped bytes of `" 然而"` to the token for `"人类"`).
4. Regardless of space prefixes, it builds a CJK punctuation cache to register all standard CJK characters.
5. It serializes both tables into a unified format:
   ```
   [uint32_t magic: "STRP"] [uint32_t n_vocab] [vector<llama_token> strip_map] [vector<uint8_t> is_cjk_punct_cache]
   ```

---

## 3. Run Inference with Runtime Injection (Online Step)

Pass the `--cjk-strip-map` CLI parameter to target binaries to automatically load the map at startup.

Using `llama-cli` as an example:

```bash
./bin/llama-cli \
    --model ~/gguf/gemma-4-12B-it-Q4_K_M.gguf \
    --cjk-strip-map gemma-4_cjk_strip.bin \
    --prompt "你认识上海迪士尼乐园的玲娜贝儿吗？她的英文名是Linabell，是一只粉色的狐狸。" \
    --predict 64
```

Alternatively, running the interactive conversation mode via `llama-completion`:

```bash
./bin/llama-completion \
    --model ~/gguf/gemma-4-12B-it-Q4_K_M.gguf \
    --cjk-strip-map gemma-4_cjk_strip.bin \
    --prompt "你认识上海迪士尼乐园的玲娜贝儿吗？它的英文名是Linabell，是一只粉色的狐狸。" \
    --system-prompt "Always respond natively and fluently in Simplified Chinese." \
    --jinja \
    --predict 400
```

### How the runtime interceptor works:
* At startup, the mapping is parsed exactly once and stored safely inside `common_params_sampling`.
* Inside `common_sampler_init`, the loaded map size is verified against the model's vocabulary size (`GGML_ASSERT(map_size == n_vocab)`).
* In the hot loop (`common_sampler_sample`), if the previous token was a CJK punctuation/character (determined in $O(1)$ from `cjk_punct_cache`), and the sampled token is mapped to a prefix-stripped counterpart in `cjk_strip_map`, the sampler automatically substitutes the token in $O(1)$ before committing it.
* Because the data is copied per-sampler instance, this approach is **fully thread-safe** and works seamlessly in highly concurrent environments like `llama-server`.

---

## 4. Heuristic Self-Speculative Rollback

To handle isolated "pure space" tokens without forcibly truncating the model's expected probability manifold with `-INFINITY` logit rejection (which causes syntactic collapse), the system employs a sliding-window speculative rollback at the application layer (`llama-completion`):

* **Sliding Window Tracking**: Maintains a 3-token history `[Token A, Token B, Token C]` during generation.
* **Pattern Detection**: Matches when `Token A` (CJK punct), `Token B` (Space), and `Token C` (CJK character) are generated consecutively.
* **KV Cache & Decoder Rollback**:
  - Removes `Token B` and `Token C` from the KV cache using `llama_memory_seq_rm(mem, 0, n_past - 2, -1)`.
  - Rewinds `n_past -= 2` and re-decodes `Token C` (rescued CJK character) at the correct position.
* **Console & Buffer Synchronization**:
  - Dynamically calculates the UTF-8 visual column widths of the space and CJK character.
  - Backspaces exactly `space_width + cjk_width` columns from the terminal to erase the space and Token C, then reprints Token C.
  - Removes Token B and Token C from local buffers (`output_tokens`, `output_ss`, `assistant_ss`) and appends rescued Token C.
* **Sampler State Rewind**: Calls `common_sampler_pop(smpl, 2)` to pop 2 tokens from the sampler's internal Markov chain and accepts rescued Token C.

