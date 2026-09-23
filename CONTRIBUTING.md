# 参与贡献

感谢你为 InSoulForge 提交改进。本文说明提交 Issue、修改代码和创建 Pull Request
的约定；本地环境、构建命令和项目架构见[开发文档](docs/DEVELOPMENT.md)。

## 开始前

1. 搜索现有 Issue 和 Pull Request，避免重复工作。
2. 对功能较大、会改变消息处理行为或配置格式的改动，先创建 Issue 说明目标、方案和兼容性影响，达成共识后再实现。
3. 不要提交 API Key、Access Token、QQ 号、聊天记录、数据库、日志或本地配置。程序运行生成的 `data/config.json` 仅用于本地或部署环境。

## 提交 Issue

请提供可复现、可判断的问题描述：

- Bug：版本、运行环境、复现步骤、预期行为、实际行为，以及相关日志（必须移除密钥和个人信息）。
- 功能建议：要解决的问题、预期使用方式、可能影响的模块。
- 安全问题：不要在公开 Issue 中披露可被利用的细节；请联系维护者处理。

## 本地开发

1. Fork 本仓库并克隆自己的 Fork。
2. 从最新默认分支创建主题分支，例如 `feat/message-recall` 或 `fix/websocket-reconnect`。
3. 按[开发文档](docs/DEVELOPMENT.md#环境要求)安装依赖并完成首次构建。
4. 保持一次改动只解决一个问题。不要在功能或修复提交中混入无关重构、格式化或生成文件。

完成修改后，至少执行：

```bash
clang-format -i <修改的 C++ 文件>
cmake --build cmake-build-debug --target insoulforge -j8
```

`insoulforge` 目标会自动执行前端构建，无需额外运行 `npm run build`。涉及后端消息契约时，额外执行：

```bash
cmake --build cmake-build-debug --target insoulforge_message_contract_tests -j8
ctest --test-dir cmake-build-debug --output-on-failure
```

修改前端的 TypeScript 类型或接口时，可额外执行：

```bash
cd frontend
npm run type-check
```

测试尚不能覆盖的行为，请在 Pull Request 中明确说明手动验证方式和剩余风险。

## 代码要求

- 遵循[编码规范](docs/CODING_STYLE.md)，尤其是命名、`clang-format`、AAA 类型推导、Doxygen 注释和 `drogon::Task` 协程参数规则。
- 保持模块职责清晰。优先在现有功能模块中扩展，不因单个需求引入无明确收益的抽象层。
- 对用户可见行为、配置结构、数据库结构、消息工作流或工具调用语义的改动，必须同步更新相关文档和测试。
- 新增或修改持久化数据时，考虑旧数据兼容、迁移和失败恢复；新增配置时提供默认值与校验。
- 修改日志时不得输出密钥、令牌、Cookie、完整授权头或不必要的用户隐私数据。

## 提交信息

提交信息使用 Conventional Commits 风格，范围使用受影响模块的小写名称，摘要使用简洁中文且不加句号：

```text
feat(media): 支持图片描述缓存过期清理
fix(scheduler): 修复定时任务异常延迟触发
docs(contributing): 补充外部贡献流程
```

常用类型为 `feat`、`fix`、`refactor`、`docs`、`test`、`build` 和 `chore`。一次提交应保持可构建；无关格式化应单独提交。

## Pull Request

创建 Pull Request 前，请确认：

- 分支基于最新默认分支，且没有无关提交或合并提交。
- 本地构建、相关测试和前端检查均已通过。
- 标题使用与提交信息一致的格式。
- 描述包含改动目的、实现要点、测试命令及结果。
- 涉及管理后台界面时附上修改前后的截图；涉及行为变更时说明兼容性和配置影响。

维护者会重点检查正确性、并发与生命周期安全、配置和数据兼容性、测试覆盖，以及是否与现有架构保持一致。请通过新增提交或整理后的分支更新处理评审意见；不要强制推送默认分支。

## 行为与许可证

讨论应围绕代码和问题本身，尊重其他贡献者。提交 Pull Request
即表示你有权提交该内容，并同意贡献以本项目的 [AGPL-3.0-only](LICENSE) 许可证发布。
