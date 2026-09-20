---
name: release
description: 发布 drogon-pay：归档 CHANGELOG、同步三处版本号声明、打 tag；GitHub Release 由 CI 自动创建
disable-model-invocation: true
---

# Release 发布技能

把发布做成**可验证**的动作：三处版本号声明同步、`CHANGELOG.md` 有对应版本段，
tag 推出后由 `.github/workflows/release.yml` 完成消费者验证与 GitHub Release。

## 使用时机

`master` 的 CI 已绿、功能已冻结，需要发布新版本时使用 `/release`。

## 版本号存在哪里

版本只**声明**、不派生，且只允许三处（SemVer `MAJOR.MINOR.PATCH`）：

| 位置 | 字段 |
|------|------|
| `CMakeLists.txt`（仓库根） | `project(drogon-pay VERSION x.y.z …)` |
| `conanfile.py` | `version = "x.y.z"` |
| `examples/pay-admin/package.json` | 顶层 `"version"` |

`libs/drogon-pay/CMakeLists.txt` 以 `${PROJECT_VERSION}` 引用，不要在那里另写一个数字。
`scripts/check_version_sync.py` 是这件事的门禁：不带参数断言三处一致（`static-analysis`
每个 PR 跑一次）；`--tag vX.Y.Z` 额外要求 tag 等于三处声明，且 `CHANGELOG.md` 已有
`## [x.y.z]` 段。

检查器**不看文档**，所以还有一类人肉同步点：`README.md`、`README.zh-CN.md`、
`docs/development/plugin_integration.md` 里那 6 处 `drogon-pay/1.0.0` 是**已发布包的
引用示例**（消费者 `self.requires(...)` 抄的就是它）。它们指的是上一个已 tag 的版本，
所以下一次 `release.yml` 发布成功后，必须在同一个发布 PR 里把它们改成新版本号——
漏掉的话文档会让人 `requires` 一个根本还没发布的包。

## 发布流程

### 1. 前置确认

```bash
git status --short                     # 必须为空
git log --oneline -1 origin/master     # 发布内容必须已经在 master 上
```

### 2. 先归档 CHANGELOG，再改版本号

把 `## [Unreleased]` 的内容移入带日期的版本段，并留一个空的 `[Unreleased]`：

```markdown
## [Unreleased]

## [1.1.0] - 2026-09-18

### Added
...
```

顺序很重要：版本段必须先于 tag 存在，`version-check` 才有东西可校验。

### 3. 同步三处版本号

同一个提交里改上表三处。**不要**在配置、部署或告警文件的注释里复述版本号——
那六处注释正是这套检查器存在的原因。

### 4. 本地预演发布门

```bash
python3 scripts/check_version_sync.py
python3 scripts/check_version_sync.py --tag v1.1.0
```

两条都必须 exit 0，否则不要往下走。

### 5. 走 PR，不要在功能分支上打 tag

```bash
git checkout -b release/v1.1.0
git commit -am "build(release): prepare v1.1.0"
git push -u origin release/v1.1.0      # push 前需要人工评审
```

等三平台 MAIN 门全绿后合并。

### 6. 在 master 上打 tag 并推送

```bash
git checkout master && git pull --ff-only
git tag -a v1.1.0 -m "Release v1.1.0"
git push origin v1.1.0
```

推送 `v*` 会自动触发 `.github/workflows/release.yml`：

| Job | 内容 |
|-----|------|
| `version-check` | 第 4 步的两条断言在 CI 里再跑一次（秒级，失败即中止，不浪费构建时间） |
| `ci-gate` | 门禁本体在 `.github/workflows/_tag-gate.yml`（`workflow_call`），此处只是被调用。先确认 tag 指向的 commit **已在 `master` 历史上**，再轮询合并流水线在该 commit 上报的五个 context（三条 `* / build-test` + 两条 `* / sdk-smoke`），全绿才放行；同名被多次上报时按 id 最大的那次判定（重跑后的旧红不算数），任一红立刻中止，最长等 `DEADLINE_MINUTES: 120` 分钟；一个 check run 都没有的 commit 5 分钟即早退（多 commit push 只在 tip 触发 CI） |
| `sdk-smoke` | Linux + Windows 各一次 `conan create` + `test_package`，复用 RELEASE 门同一份 `_sdk-smoke.yml` |
| `publish` | 取 `CHANGELOG.md` 对应版本段作为正文，`gh release create` |

`_tag-gate.yml` 存在的原因：ruleset 的必需检查只管**合并**，tag 不是被合并进来的，所以
`git tag v1.1.0 <任意 commit>` 过去可以绕过整套门禁——`v1.0.0` 就是在
`windows-build-and-test` 为 failure 的情况下发布出去的，而 `deploy.yml` 同样吃 `v*`
push，任意 tag 还能直接把镜像推出去、滚动 production ECS。工作流之间的 `needs` 不成立，
所以修法不是让 release.yml 去管 deploy.yml，而是两个调用同一份门禁：`release.yml` 的
`ci-gate` 与 `deploy.yml` 的 `tag-gate`。因此 tag 必须打在 master 上 CI 已跑完的
commit（正常顺序就是合并后直接在 master tip 打 tag）；如果刚合并就想打 tag，门禁会替
你在原地等那一轮转完（最长 `DEADLINE_MINUTES`，两小时）。`deploy.yml` 从分支
`workflow_dispatch` 时没有 tag 可证明，门禁原样放行——这是它仍能用于手动部署的原因。

**不要再手工 `gh release create`**：release 由流水线创建，手工创建会撞名失败。

### 7. 确认

```bash
gh run list --workflow=release.yml --limit 1
gh release view v1.1.0
```

## 版本号规范

遵循 Semantic Versioning：

- **MAJOR**：不兼容的 API/ABI 变更（SPI 签名、公开头文件语义）
- **MINOR**：向下兼容的功能新增
- **PATCH**：向下兼容的缺陷修复

破坏性变更在 Conventional Commit 的 scope 后加 `!`（`feat(spi)!: …`），body 里写
`BREAKING:`，并在同一 PR 更新 `docs/development/plugin_integration.md` 的兼容表。

预发布：发布门只接受三段数字版本，`v1.1.0-rc.1` 这类 tag 会被
`check_version_sync.py --tag` 拒绝。需要预发布级别的消费者验证时直接在 PR 分支上跑
——`ci.yml` 的 RELEASE 门用的就是同一份 `sdk-smoke`。

## 回滚计划

已发布的 tag 不移动（发布语义要求可追溯）：

```bash
gh release delete v1.1.0 --yes         # 撤下 release，保留 tag
git push origin :refs/tags/v1.1.0      # 仅当该版本从未被消费过才删 tag
git tag -d v1.1.0
```

修复走新提交 + 新 PATCH 版本（分支 `release/v1.1.1`），不要回滚已合并的 master 提交。

## 发布检查清单

- [ ] `master` 三平台 MAIN 门全绿
- [ ] tag 打在 CI 已跑完的 master commit 上（`ci-gate` 会校验，不等它也要过它）
- [ ] `check_version_sync.py --tag v<version>` 本地通过
- [ ] `CHANGELOG.md` 已有该版本段且日期正确
- [ ] release.yml 的 `sdk-smoke`（Linux + Windows）通过
- [ ] GitHub Release 已创建、正文来自 CHANGELOG 版本段
- [ ] `README.md`、`README.zh-CN.md`、`docs/development/plugin_integration.md` 里
      6 处 `drogon-pay/<version>` 已改成刚发布的版本号
