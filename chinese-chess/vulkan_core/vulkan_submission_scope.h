/**
 * @file vulkan_submission_scope.h
 * @brief 提交失败时的异常安全护栏：先停提交，再排空已发布 token。
 * @details 构造时记录 uncaught_exceptions 计数，只有异常展开（计数增加）才触发清理，正常退出零开销。
 *          清理包括 stop_submissions() 与两个队列最新 token 的 wait_for_cleanup()。
 *          这样被展开的局部资源不会在 GPU 仍可能写它时析构。
 */
#pragma once

#include "vulkan_queue.h"

#include <exception>

// Declare AFTER the local resources that may be referenced by a submission.
// Normal exit is non-blocking. Failure drains only published tokens before
// those resources unwind, and forbids reuse of speculative resource states.
/**
 * @brief 作用域级的提交回滚守卫，必须声明在可能被提交引用的局部资源之后。
 * @details 正常退出不做任何事；异常展开时先禁止队列继续提交，再排空已发布 token 才让局部资源析构。
 *          两个队列可以是同一个 VkQueue（别名队列），此时 stop 与 wait 重复执行也是幂等的。
 */
class vulkan_submission_scope
{
public:
    /**
     * @brief 记录两个队列与当前未捕获异常计数。
     * @param _owner 拥有资源所有权的队列，异常时先停它的提交。
     * @param _transfer 执行传输的队列，可与 _owner 是同一个 VkQueue。
     * @note 只保存引用，队列必须比本守卫活得久；构造不查询也不等待。
     */
    vulkan_submission_scope(const vulkan_queue& _owner, const vulkan_queue& _transfer) noexcept
        : owner(_owner)
        , transfer(_transfer)
        , exceptions(std::uncaught_exceptions())
    {
    }
    /** @brief 禁止拷贝构造：守卫绑定两个队列，不可复制。 */
    vulkan_submission_scope(const vulkan_submission_scope&) = delete;
    /** @brief 禁止拷贝赋值：守卫绑定两个队列，不可复制。 */
    vulkan_submission_scope& operator=(const vulkan_submission_scope&) = delete;
    /**
     * @brief 异常展开时停提交并排空 token，正常退出无操作。
     * @note 以构造时记录的 uncaught_exceptions 为基线，只有计数增加才清理；析构不抛异常。
     */
    ~vulkan_submission_scope()
    {
        if (std::uncaught_exceptions() > exceptions)
        {
            owner.stop_submissions();
            transfer.stop_submissions();
            owner.get_latest_token().wait_for_cleanup();
            transfer.get_latest_token().wait_for_cleanup();
        }
    }

private:
    /** @brief 拥有资源的队列引用，异常时先停它的提交。 */
    const vulkan_queue& owner;
    /** @brief 传输队列引用，与 owner 可能指向同一个 VkQueue。 */
    const vulkan_queue& transfer;
    /** @brief 构造时的 std::uncaught_exceptions() 计数，用于区分正常退出与异常展开。 */
    int exceptions;
};
