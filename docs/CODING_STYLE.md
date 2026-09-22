# insoulforge 编码规范

本文档规定项目新增或修改 C++ 代码的默认写法。优先级从高到低为：正确性、清晰性、与现有模块一致性、简洁性。第三方框架要求的签名可以例外，但例外应限制在边界处。

## 基本原则

- 使用 C++23 和项目根目录的 `.clang-format`；提交前对修改的 C++ 文件执行 `clang-format`。
- 代码应表达业务意图，不依赖注释解释显而易见的语法行为。
- 保持单一职责。一个函数完成一个明确动作；复杂流程应拆为具名的私有函数，而不是堆叠嵌套条件。
- 优先值语义、RAII 和标准库类型；避免手动资源管理、裸 `new`、裸 `delete` 和不必要的共享所有权。
- 除 `#pragma once`、第三方库集成等不可避免的场景外，不新增宏。常量使用 `constexpr`，泛型使用模板或概念。
- 不为潜在需求预先抽象。只有重复逻辑、明确的扩展点或边界隔离需要时才引入接口或类。

## 命名

名称必须描述职责或数据含义，不使用无语义缩写。布尔变量与布尔函数应能直接读成判断句。

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| 头文件、源文件 | `PascalCase.hpp`、`PascalCase.cpp` | `OneBotEventWorkflow.hpp` |
| 类型、枚举、别名 | `PascalCase` | `MessageList`、`ToolCategory` |
| 枚举值 | 枚举内保持一致 | `Level::Warning` 或 `ToolCategory::INFORMATION` |
| 函数、方法、局部变量、参数 | `camelCase` | `processMessage`、`groupId` |
| 成员变量 | `m_` + `camelCase` | `m_sessionId` |
| 静态数据成员 | `s_` + `camelCase` | `s_instance` |
| 命名常量 | `k` + `PascalCase` | `kMaxRetries` |
| 命名空间 | 全小写 | `insoulforge` |

补充规则：

- 返回布尔值的函数使用 `is`、`has`、`can`、`should` 等前缀，例如 `isEnabled()`、`hasConfig()`。
- 执行动作的函数使用动词，例如 `loadConfig()`、`sendMessage()`；只读访问器可使用名词，例如 `snapshot()`、`sessionId()`。
- 新增枚举值优先使用 `PascalCase`。既有公开枚举保持原有风格，不为命名格式单独制造兼容性变更；同一枚举内禁止混用风格。
- 命名空间使用连续的小写单词，不使用下划线或大小写混合。
- 新代码不定义可变全局变量。进程级对象应由明确的所有者管理；确有必要的全局常量使用命名空间内的 `inline constexpr`。
- 接口或抽象类仅在多实现确实存在时创建。需要时使用 `I` 前缀，例如 `IMessageSender`；异常类型以 `Exception` 结尾。

## 格式与结构

### 格式化和包含

`.clang-format` 是唯一格式化来源。当前项目基于 LLVM 风格，使用 4 空格缩进和 120 列行宽；不要在代码中手工对齐来对抗格式化工具。

包含顺序如下，每组之间保留一个空行：

1. C++ 标准库头文件。
2. 第三方库头文件。
3. 项目内部头文件。

```cpp
#include <string>
#include <vector>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
```

所有头文件使用 `#pragma once`。头文件应最小化依赖：能前置声明时不包含完整定义；实现文件包含所需的完整定义。

### 初始化与指定初始化器

优先使用列表初始化，避免窄化转换。多行指定初始化器末尾必须保留尾随逗号，以保证 `clang-format` 保持一项一行的稳定布局。

```cpp
registry.registerTool(
    {
        .name = "deep_think",
        .description = "工具描述，LLM 据此判断何时调用",
        .parameters = thinkParams,
        .handler = [](json) -> drogon::Task<std::string> { co_return "ok"; },
    },
    ToolCategory::INFORMATION);
```

## 类型与所有权

### Almost Always Auto（AAA）

局部变量默认使用 **Almost Always Auto（AAA）**。当初始化表达式或右侧显式类型足以说明结果类型时，使用 `auto`、`const auto` 或 `const auto &`，避免重复类型、意外转换和错误的引用语义。

```cpp
const auto sessionId = message.at("group_id").get<uint64_t>();
auto response = co_await client.sendRequest(std::move(request));
const auto &config = Config::instance().llm();
auto retryPolicy = RetryPolicy{.maxAttempts = 3, .retryDelay = std::chrono::seconds{1}};
```

需要明确构造目标类型时，使用 `auto variable = Type{...}`：类型只出现一次，同时保留构造意图。

```cpp
auto messageIds = std::vector<uint64_t>{firstId, secondId};
```

花括号会优先匹配 `std::initializer_list`。若这会改变构造语义，必须使用圆括号选择目标构造函数：

```cpp
auto retryDelays = std::vector<std::chrono::seconds>(3, std::chrono::seconds{1});
```

上例创建三个一秒的延迟项；若使用花括号，会创建包含两个元素的 `initializer_list` 容器。

以下情况显式写出类型更清晰：

- 初始化表达式无法看出类型，且变量名无法消除歧义。
- 基础数值或布尔值的类型本身表达单位、范围或业务含义。
- 需要明确窄化、类型转换或重载选择。
- 函数返回类型、公开成员、接口参数和序列化模型等对外契约。AAA 只适用于局部变量。

`auto` 不会保留引用或 `const`，除非显式写出。只读借用使用 `const auto &`；需要取得所有权或延长临时对象生命周期时使用 `auto` 或 `const auto`。不得因使用 `auto` 意外复制大型对象或丢失引用语义。

### 参数与返回值

- 只读且可复制成本较高的普通参数使用 `const T &`。
- 函数需要取得所有权，或参数将被移动到返回值、成员或异步任务中时按值接收，并在函数体内 `std::move`。
- 输出结果通过返回值、结构体或 `std::expected` 表达；不使用跨调用链的非 `const` 引用作为输出参数。
- 只读查询且调用方不应忽略结果时使用 `[[nodiscard]]`。
- 成员函数不修改对象状态时声明为 `const`；不会抛出且该保证稳定时声明为 `noexcept`。

## Doxygen 注释

注释说明“为什么、约束和边界”，不复述代码已经表达的“做什么”。对外头文件、公共类型、非直观的公共函数，以及并发或生命周期敏感的接口必须使用 Doxygen 注释。

```cpp
/// @file MessageList.hpp
/// @brief 维护单个会话的完整消息记录。

/// @brief 获取模型可见的近期消息快照。
/// @return 按时间顺序排列的消息 JSON 数组。
/// @note 线程安全。返回值不受后续消息写入影响。
[[nodiscard]] json snapshot() const;
```

按需使用以下标签：

- `@param`：参数含义、单位、所有权或有效范围不明确时。
- `@return`：返回值语义、空值和错误状态不直观时。
- `@throws`：异常类型或抛出条件对调用方有影响时。
- `@pre`、`@post`：调用前提和完成后的状态约束。
- `@note`：线程安全、生命周期、性能或副作用等重要限制。

成员变量仅在含义不能从名称和类型直接看出时使用行尾 `///<` 注释。

## 协程参数（`drogon::Task`）

协程可以在调用方栈帧结束后继续运行，因此参数的生命周期必须覆盖整个协程。`drogon::Task` 的值语义数据必须由协程帧持有，不能把调用方局部对象的引用或指针跨越挂起点保存。

1. `json`、`std::string`、容器和业务结构体等值语义参数一律按值传入协程；调用后不再使用实参时传入 `std::move(value)`，否则传入 lvalue 让协程复制。
2. 禁止从单例、配置成员或会被复用的 lambda 捕获中移动数据。
3. 普通非协程函数不受此限制：只读大对象仍使用 `const T &`；消费型函数可按值接收后移动。
4. 仅当引用对象的生命周期明确覆盖整个协程，且调用方会 `co_await` 到完成时，才允许协程接收 `const T &`。进程级单例成员和不可复制的会话级管理器属于此例外。
5. `std::string_view` 只能接收生命周期覆盖协程全程的字符串字面量、静态字符串或受同一持有者管理的字符串。
6. 框架固定签名无法改为值参数时，在进入协程后、首次挂起前复制需要保留的数据。
7. 不使用 `T &` 作为跨协程输出参数；通过返回值或聚合返回类型传回结果。

项目通过 `JSON_USE_IMPLICIT_CONVERSIONS=0` 禁用了 nlohmann/json 的隐式转换。这一配置是协程稳定性的必要条件，不得移除或覆盖。
