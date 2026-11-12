# AI 资产与守卫脚本

<cite>
**Referenced Files**
- [AGENTS.md](file://AGENTS.md)
- [check_docs_drift.py](file://scripts/check_docs_drift.py)
</cite>

## .claude 资产清单

`AGENTS.md` 的 Claude Code Assets 节是权威清单，`check_docs_drift.py` R1 强制清单与磁盘一致。tombstone 条目（front matter `status: deprecated`）是退役的 authforge/OAuth2 遗留占位，**永不复活其内容**。

| 类别 | 成员 |
|------|------|
| Agents | api-documenter、ci-monitor、code-reviewer、compliance-checker[retired]、payment-test-reviewer、performance-analyzer、security-reviewer、test-writer |
| Rules | data-access、db-operations、dev-workflow、orm-models |
| Skills | build-and-test、create-migration、db-reset、docker-integration-test、docker-manage、drogon-build、e2e-test[retired]、openapi-update、orm-gen、production-readiness-docs、project-conventions、release |

`.codex/` 是 `.claude/` 的镜像（第二个代理用）：同名文件必须字节一致（R5），改 .claude 后同提交 cp 过去。

**Sources** · [AGENTS.md](file://AGENTS.md) · [check_docs_drift.py](file://scripts/check_docs_drift.py)

## 守卫脚本日常使用

推送前本地全跑（都是 stdlib-only）：

```bash
python scripts/check_architecture.py
python scripts/check_openapi_routes.py
python scripts/check_version_sync.py
python scripts/check_migrations.py
python scripts/check_docs_drift.py
python scripts/check_test_layout.py
python scripts/ci/version_sync_scenarios.py
python scripts/ci/tag_gate_scenarios.py
```

check_docs_drift 七规则：R1 资产清单、R2 反引号路径可解析（对 git index + gitignore，不是本地磁盘）、R3 禁 gtest 词汇、R4 迁移引用存在、R5 孪生镜像字节一致、R6 禁手写版本/日期戳、R7 入口脚本孪生完整。

**Sources** · [check_docs_drift.py](file://scripts/check_docs_drift.py) · [CONTRIBUTING.md](file://CONTRIBUTING.md)

## 迁移与 ORM 工作流

- 新迁移：`/create-migration` 技能清单（命名 NNN_snake_case、幂等非破坏、同事务记账）；发布后 `check_migrations.py --write-missing` 钉基线；
- ORM 重生：`/orm-gen`（drogon_ctl），模型禁手改；
- 开发重置：`/db-reset`（setup_database 孪生脚本，000 是重置助手不是版本）；
- 契约变更：`/openapi-update`（openapi.yaml 与代码同步，check_openapi_routes 双向比对）。

**Sources** · [SKILL.md](file://.claude/skills/create-migration/SKILL.md) · [SKILL.md](file://.claude/skills/orm-gen/SKILL.md) · [SKILL.md](file://.claude/skills/openapi-update/SKILL.md)
