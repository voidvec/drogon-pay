#pragma once

// ============================================================================
// TECHNICAL DEBT NOTICE
// ============================================================================
// This service was NOT implemented using Test-Driven Development (TDD).
//
// Created: 2026-04-11
// TDD Status: Non-compliant
// Test Coverage: Integration tests only (no unit tests)
//
// Issues:
// - Implemented without failing tests first
// - Unit tests were removed due to being incomplete (nullptr mocks)
// - Behavior verified through integration tests only
//
// Priority: Medium
// Planned Action: Reimplement with TDD when time permits
//
// Integration Test Status: See test/ directory for integration tests
// that verify this service's behavior.
//
// Note: When modifying this service, consider adding TDD-compliant
// unit tests for new functionality.
// ============================================================================

#include <drogon/orm/DbClient.h>
#include <drogon/nosql/RedisClient.h>
#include "IdempotencyService.h"
#include "drogon_pay/PaymentChannel.h"
#include <json/json.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

struct CreateRefundRequest
{
    std::string orderNo;
    std::string paymentNo;
    std::string refundNo;
    std::string amount;
    std::string reason;
    std::string notifyUrl;
    std::string fundsAccount;
};

class RefundService
{
  public:
    using RefundCallback =
      std::function<void(const Json::Value &result, const std::error_code &error)>;

    RefundService(
      std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
      std::shared_ptr<drogon::orm::DbClient> dbClient,
      std::shared_ptr<IdempotencyService> idempotencyService
    );

    void createRefund(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      RefundCallback &&callback
    );

    void queryRefund(const std::string &refundNo, RefundCallback &&callback);

    void syncRefundStatusFromWechat(
      const std::string &refundNo,
      const Json::Value &result,
      std::function<void(const std::string &status)> &&callback
    );

  private:
    // Channel name -> SPI implementation; unknown channels fail explicitly.
    std::map<std::string, drogon_pay::PaymentChannelPtr> channels_;
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    std::shared_ptr<IdempotencyService> idempotencyService_;

    drogon_pay::PaymentChannelPtr findChannel(const std::string &name) const;

    // Helper methods for refund processing
    void proceedRefund(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      RefundCallback &&callback
    );

    void proceedOrderFlow(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const std::string &refundNo,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      const std::string &reason,
      RefundCallback &&callback
    );

    void proceedWithAmountCheck(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const std::string &refundNo,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      int64_t refundFen,
      int64_t totalFen,
      const std::string &currency,
      const std::string &reason,
      RefundCallback &&callback
    );

    void proceedWithInProgressCheck(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const std::string &refundNo,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      int64_t refundFen,
      int64_t totalFen,
      const std::string &currency,
      const std::string &reason,
      RefundCallback &&callback
    );

    void proceedWithInsert(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const std::string &refundNo,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      int64_t refundFen,
      int64_t totalFen,
      const std::string &currency,
      const std::string &reason,
      RefundCallback &&callback
    );

    void invokeRefundChannel(
      const CreateRefundRequest &request,
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const std::string &refundNo,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      int64_t refundFen,
      int64_t totalFen,
      const std::string &currency,
      const std::string &reason,
      const std::string &channel,
      RefundCallback &&callback
    );

    void updateRefundWithError(
      const std::string &refundNo,
      const std::string &errorMessage,
      const Json::Value &errJson
    );

    void updateRefundWithSuccess(
      const std::string &refundNo,
      const std::string &refundStatus,
      const std::string &refundId,
      const Json::Value &result,
      const std::string &orderNo,
      const std::string &paymentNo,
      const std::string &amount,
      int64_t orderTotalFen,
      RefundCallback &&callback
    );
};
