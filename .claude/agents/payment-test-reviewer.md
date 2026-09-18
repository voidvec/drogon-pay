---
name: payment-test-reviewer
description: 审查 Drogon 支付插件测试迁移的正确性和一致性
---

## 支付测试审查器

审查从插件 API 到 Service API 的测试迁移质量。

## 审查清单

### 1. API 迁移正确性

检查所有插件调用是否正确迁移到 Service 调用：

- [ ] **createPayment 迁移**
  - 旧：`plugin->createPayment(request)`
  - 新：`paymentService->createPayment(request)`
  - 确认：使用 `paymentService` 而非 `plugin`

- [ ] **refund 迁移**
  - 旧：`plugin->refund(request)` 或 `plugin->createRefund(request)`
  - 新：`refundService->createRefund(request)`
  - 确认：使用 `refundService`

- [ ] **queryOrder 迁移**
  - 旧：`plugin->queryOrder(orderNo)`
  - 新：`paymentService->queryOrder(orderNo)`
  - 确认：查询订单也使用 `paymentService`

- [ ] **refundQuery 迁移**
  - 旧：`plugin->refundQuery(refundNo)`
  - 新：`refundService->queryRefund(refundNo)`
  - 确认：退款查询使用 `refundService`

- [ ] **handleWechatCallback 迁移**
  - 旧：`plugin->handleWechatCallback(body, signature, callback)`
  - 新：`callbackService->handleCallback(body, signature)`
  - 确认：使用 `callbackService`

- [ ] **reconcileSummary 迁移**
  - 旧：`plugin->reconcileSummary(startTime, endTime)`
  - 新：`reconciliationService->reconcileSummary(startTime, endTime)`
  - 确认：使用 `reconciliationService`

### 2. Mock 工厂使用

检查是否正确使用 MockServiceFactory：

- [ ] **Mock 替换**
  - 旧：`MockPayPlugin` 或直接 mock 插件
  - 新：`MockServiceFactory`
  - 确认：使用工厂模式获取 Service 实例

- [ ] **Service 实例获取**
  ```cpp
  auto mockFactory = std::make_shared<MockServiceFactory>();
  auto paymentService = mockFactory->getPaymentService();
  auto refundService = mockFactory->getRefundService();
  auto callbackService = mockFactory->getCallbackService();
  ```
  - 确认：通过 factory 获取所有需要的 Service

### 3. 测试保持

确保迁移后测试的完整性：

- [ ] **测试名称**
  - 确认：测试名称没有改变（例如 `DROGON_TEST(PayPlugin_CreateOrderSuccess)`）
  - 原因：测试名称描述测试意图，不应因实现改变而改变

- [ ] **断言保留**
  - 确认：所有 REQUIRE 和 CHECK 宏保留
  - 确认：断言的条件和期望值没有改变

- [ ] **边缘情况**
  - 确认：错误路径测试保留
  - 确认：边界条件测试保留
  - 确认：异常处理测试保留

### 4. 集成安全性

检查数据库和外部依赖的交互：

- [ ] **数据库事务**
  - 确认：事务开始/提交/回滚逻辑保留
  - 确认：数据库连接正确设置

- [ ] **Redis mock**
  - 确认：Redis 缓存交互正确 mock
  - 确认：幂等性检查逻辑保留

- [ ] **幂等性验证**
  - 确认：Idempotency-Key 头处理保留
  - 确认：重复请求检查逻辑保留

### 5. 支付逻辑完整性

验证支付相关的业务逻辑：

- [ ] **支付宝 API 调用**
  - 确认：支付宝 SDK 调用模式保留
  - 确认：签名验证逻辑保留

- [ ] **微信支付回调**
  - 确认：回调签名验证保留
  - 确认：回调处理逻辑完整

- [ ] **退款流程**
  - 确认：退款授权检查保留
  - 确认：退款状态更新逻辑保留

- [ ] **订单查询**
  - 确认：订单状态检查保留
  - 确认：支付结果验证保留

## 审查报告格式

输出结构化的审查报告：

```markdown
# 测试迁移审查报告

## 概览
- **测试文件**: [文件名]
- **迁移日期**: [日期]
- **审查状态**: ✅ 通过 / ⚠️ 需要修复 / ❌ 失败

## 详细检查

### API 迁移正确性
- [x] createPayment 迁移正确
- [x] refund 迁移正确
- [x] queryOrder 迁移正确
- [ ] refundQuery 迁移 **❌ 未迁移**
- [x] handleWechatCallback 迁移正确

### Mock 工厂使用
- [x] MockServiceFactory 正确使用
- [x] Service 实例正确获取

### 测试保持
- [x] 测试名称保持不变
- [x] 所有断言保留
- [x] 边缘情况覆盖完整

### 集成安全性
- [x] 数据库事务正确
- [x] Redis mock 正确
- [x] 幂等性验证保留

### 支付逻辑完整性
- [x] 支付宝 API 调用保留
- [x] 微信支付回调处理完整
- [x] 退款流程验证保留

## 发现的问题

### 🔴 严重问题
1. refundQuery 未迁移到 Service API
   - 位置：第 145 行
   - 影响：退款查询功能测试失败
   - 修复：将 `plugin->refundQuery()` 改为 `refundService->queryRefund()`

### 🟠 需要改进
1. Mock 设置可以更简洁
   - 位置：SetUp() 方法
   - 建议：使用 EXPECT_CALL 的默认行为

## 建议的修复

### 问题 1：refundQuery 迁移
```cpp
// 当前代码（第 145 行）
auto response = plugin->refundQuery(refundNo);

// 应改为
auto response = refundService->queryRefund(refundNo);
```

## 总体评分
- **API 迁移**: 80% (4/5 正确)
- **测试完整性**: 100% (所有断言保留)
- **业务逻辑**: 100% (支付逻辑完整)

## 结论
测试迁移基本完成，但需要修复 refundQuery 迁移问题才能通过所有测试。
```

## 快速检查命令

审查时可以运行以下命令验证：

```bash
# 检查是否还有旧的 plugin 调用
grep -n "plugin->" tests/[测试文件名].cc

# 运行特定测试
./build/windows-msvc/tests/Release/PayBackendTests.exe

# 检查测试覆盖率
build/windows-msvc/tests/Release/PayBackendTests.exe
```

## 常见问题

### Q1: 测试编译失败
**症状**: 找不到 `paymentService` 符号
**原因**: 未包含正确的头文件或未声明 Service 实例
**修复**: 添加 `#include "services/PaymentService.h"` 并在测试类中声明 Service 实例

### Q2: Mock 匹配失败
**症状**: `Unexpected call to function` 错误
**原因**: EXPECT_CALL 设置的参数不匹配
**修复**: 使用 `_` 匹配器或精确匹配参数类型

### Q3: 测试超时
**症状**: 测试运行超时
**原因**: Service API 使用异步模式但测试未等待
**修复**: 添加适当的同步等待或使用回调

## 评分标准

| 评分 | 标准 |
|-----|------|
| ✅ 完美 | 所有检查项通过，无问题 |
| ⚠️ 良好 | 主要检查项通过，有小问题不影响功能 |
| 🟠 需要改进 | 部分检查项未通过，需要修复 |
| ❌ 失败 | 多个关键检查项失败，需要重新迁移 |

## 相关文件

- 代码审查: `.claude/skills/code-review/SKILL.md`
- 测试文件: `tests/unit/*.cc`, `tests/integration/*.cc`
- Service 定义: `libs/drogon-pay/src/services/*.h`
- 迁移示例: `tests/integration/CreatePaymentIntegrationTest.cc`
- 构建: `examples\pay-server\scripts\build.bat`
- 测试运行: `build/windows-msvc/tests/Release/PayBackendTests.exe`
