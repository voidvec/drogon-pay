# CI 门禁与守卫脚本

<cite>
**Referenced Files**
- [ci.yml](file://.github/workflows/ci.yml)
- [_build-test.yml](file://.github/workflows/_build-test.yml)
- [check_architecture.py](file://scripts/check_architecture.py)
</cite>

## 三阶段流水线

```mermaid
flowchart LR
    FAST[FAST: static-analysis + clang-tidy] --> MAIN[MAIN: _build-test 三平台]
    MAIN --> REL[RELEASE: _sdk-smoke linux+windows]
```

- **FAST**：全部 check_*.py + clang-format-22 + 禁词 grep + 两个决策表回放脚本（version_sync_scenarios.py、tag_gate_scenarios.py）；不编译。
- **MAIN**：`_build-test.yml` 按平台矩阵调用，产出三个 required check。
- **RELEASE**：`_sdk-smoke.yml` 消费者 conan create + test_package，Linux 与 Windows。

## 三平台 DB 供给差异

| 平台 | 供给 | 测试范围 |
|------|------|----------|
| Linux | Docker 容器 Postgres+Redis | 构建+全量测试 |
| Windows | runner 内置 PostgreSQL + Memurai | 构建+全量测试 |
| macOS | 无 | **仅构建**（macos-14 超出 Homebrew 支持期，装库会源码编译耗尽 120 分钟超时；该 leg 只证明 arm64 clang -Werror 编译，是唯一见 clang 独有诊断的通道） |

**Sources** · [ci.yml](file://.github/workflows/ci.yml) · [_build-test.yml](file://.github/workflows/_build-test.yml)

## required check 命名契约（易踩坑）

三个 MAIN check 是分支 ruleset 的必需项，context 带 `/ build-test` 后缀：`<调用方 job 名> / build-test`（被调工作流的 job 无 name 键时取 job id）。改任一半或删上报工作流，会让 ruleset 要求无人上报的 context——保护不解除，反而反转成**永久 pending 卡死**，且 `gh pr checks` 只显示存在的 check run，读起来全绿但 mergeStateStatus=BLOCKED。核验差集：

```bash
gh api repos/:owner/:repo/rulesets/<id> --jq '.rules[] | select(.type=="required_status_checks") | .parameters.required_status_checks[].context'
gh api repos/:owner/:repo/commits/<sha>/check-runs --jq '.check_runs[].name'
```

**Sources** · [AGENTS.md](file://AGENTS.md)

## 九个守卫脚本（stdlib-only，本地可跑）

| 脚本 | 职责 |
|------|------|
| check_architecture.py | 四条依赖规则 + 4 头白名单 |
| check_openapi_routes.py | 代码↔契约双向路由比对 |
| check_version_sync.py | 四处版本声明一致（--tag 加验 CHANGELOG） |
| check_migrations.py | 迁移命名/链/基线/幂等非破坏 |
| check_docs_drift.py | 七条文档/AI 配置漂移规则（R1-R7） |
| check_test_layout.py | 测试命名与显式登记（禁 GLOB） |
| measure_coverage.py | 覆盖率棘轮 |
| ci/version_sync_scenarios.py | 版本扫描决策表回放 |
| ci/tag_gate_scenarios.py | tag 门禁判定表回放（驱动工作流自身 run: 字节） |

推送前全部跑一遍；每个都是 stdlib-only，无额外依赖。

**Sources** · [scripts/](file://scripts/) · [CONTRIBUTING.md](file://CONTRIBUTING.md)
