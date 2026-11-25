# 开发者指南

<cite>
**Referenced Files**
- [CONTRIBUTING.md](file://CONTRIBUTING.md)
- [AGENTS.md](file://AGENTS.md)
- [TECH_SPECS.md](file://TECH_SPECS.md)
</cite>

## 面向贡献者的入口

本分区汇总参与 drogon-pay 开发所需的流程与规范：

| 主题 | 内容 | 详见 |
|------|------|------|
| 协作流程 | Fork→特性分支→PR、三平台 CI 绿准入、提交约定 | [贡献流程](贡献流程) |
| 工程规范 | C++17/异步/数据访问/日志/孪生脚本 | [编码与工程规范](编码与工程规范) |
| AI 资产 | .claude agents/rules/skills、守卫脚本日常使用 | [AI资产与守卫脚本](AI资产与守卫脚本) |

## 硬约束速查（任何编码代理都适用）

- 绝不手改 `libs/drogon-pay/src/models/` ORM 模型；
- 绝不 raw SQL，用 Mapper + Criteria 异步回调；
- 绝不手写迁移文件清单，migrate_db.py 唯一执行；
- 密钥永不进源码，`__env_var:NAME__` 占位符；
- 优先异步回调，共享状态 `[sharedCb]` 捕获；
- 用 Service API，不用遗留 Plugin API；
- 提交允许，**push 需评审**；Done = 测试过 + 静态分析过 + CI 绿 + 文档更新。

**Sources** · [AGENTS.md](file://AGENTS.md) · [CONTRIBUTING.md](file://CONTRIBUTING.md)
