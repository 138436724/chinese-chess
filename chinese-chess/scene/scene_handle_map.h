/**
 * @file scene_handle_map.h
 * @brief 带世代校验的槽表：句柄到实体，槽位可复用、擦除不分配。
 * @details 槽按 vector 顺序存放，erase 只清 optional 并把下标压进 free_slots；insert 优先复用空闲槽
 *          并沿用它的世代，新追加槽的世代从 0 开始（uint32_t 世代递增最终会回绕）。live() 以视图遍历存活槽，元素是聚合体
 *          live_reference{handle, value}，既能 entry.value 读也能结构化绑定。
 *          表刻意不提供 clear()：它会让世代归零，使旧句柄复活。
 * @note 视图与 get() 的裸指针引用内部 slots，有效期到下一次 insert / erase；不要存起来跨结构变更使用。
 */
#pragma once

#include "scene_handle.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

/**
 * @brief 以槽为单位持有 T 的句柄表，负责分配、世代校验与存活遍历。
 * @details insert 返回的句柄带当前世代；erase 成功后该槽世代自增，旧句柄随即失效。
 *          追加分支在移动待插入实体前先预留 free_slots 容量并创建空槽，保证 erase 阶段不再分配。
 * @tparam T 槽内实体类型，要求可移动构造（optional::emplace 与 vector 扩容只需这一条）。
 * @tparam H tag 类型，只用于生成 handle_type，刻意不加约束：它通常是不完全类型。
 * @note 非 const 的 get() / live() 都转发到 const 版本再 const_cast，避免同一逻辑写两遍。
 */
template <typename T, typename H>
    requires(std::move_constructible<T>)
class scene_handle_map
{
public:
    /** @brief 本表使用的句柄类型，即 scene_handle<H>。 */
    using handle_type = scene_handle<H>;

    /**
     * @brief live() 视图的元素：同时给出句柄与实体引用。
     * @details 聚合体，支持 entry.handle / entry.value 访问与 auto&& [handle, value] 解构；
     *          const 视图的元素类型是 live_reference<const T>。
     * @tparam U 引用元素的类型，带 const 与否由调用 live() 时槽表的 const 性决定。
     */
    template <typename U>
    struct live_reference
    {
        handle_type handle;  ///< @brief 该槽当前的句柄（下标 + 当前世代）
        U&          value;  ///< @brief 槽内实体引用；const 性由所选 live() 重载决定
    };

    /**
     * @brief 插入一个实体，优先复用空闲槽。
     * @param _value 待插入实体，成功时被移动进槽；其移动失败时的源对象状态取决于 T 的移动构造。
     * @return 带当前世代的新句柄。
     * @throws std::bad_alloc 槽表或空闲表分配失败时传播。
     * @note 实体 emplace 失败会移除本次追加的空槽，复用分支也不会消费 free_slots；但 vector 扩容或
     *       T 的可抛移动构造不提供普遍的强异常保证，失败前还可能使旧指针失效。
     * @note 有 free_slots 时复用其末项并沿用该槽世代；否则先预留 free_slots 容量并追加空槽，
     *       避免之后的 erase 在归还 GPU 资源之后再分配。
     */
    [[nodiscard]] handle_type insert(T&& _value)
    {
        if (!free_slots.empty())
        {
            const uint32_t index = free_slots.back();

            auto& slot = slots.at(index);
            slot.value.emplace(std::move(_value));

            free_slots.pop_back();
            ++live_count;

            return handle_type{.index = index, .generation = slot.generation};
        }

        // Grow storage before consuming a resource that may already be in use
        // by the GPU. A failed allocation leaves ownership with the caller.
        // Reserve the free list here too: erase() must never allocate after
        // retiring a GPU resource, while callers rely on its noexcept contract.
        if (free_slots.capacity() <= slots.size())
        {
            const auto capacity = free_slots.capacity() * 2;
            free_slots.reserve(capacity > slots.size() ? capacity : slots.size() + 1);
        }
        slots.emplace_back();
        try { slots.back().value.emplace(std::move(_value)); }
        catch (...) { slots.pop_back(); throw; }
        ++live_count;

        return handle_type{.index = static_cast<uint32_t>(slots.size() - 1), .generation = 0};
    }

    /**
     * @brief 擦除句柄指向的存活槽：清空 optional、世代自增并归还空闲表。
     * @param _handle 目标句柄；已死或越界时什么也不做。
     * @return 是否真的擦除了一个存活槽。
     * @note 标 noexcept：free_slots 的容量在 insert 时已预留，这里不再分配。
     */
    [[nodiscard]] bool erase(handle_type _handle) noexcept
    {
        if (!is_alive(_handle))
        {
            return false;
        }

        auto& slot = slots.at(_handle.index);
        slot.value.reset();
        ++slot.generation;

        free_slots.push_back(_handle.index);
        --live_count;

        return true;
    }

    /**
     * @brief 按句柄取实体，校验下标范围、槽占用与世代。
     * @param _handle 目标句柄。
     * @return 存活槽的只读指针；句柄无效、越界、已擦除或世代不符时返回 nullptr。
     * @note 指针有效期到下一次 insert / erase。
     */
    [[nodiscard]] const T* get(handle_type _handle) const noexcept
    {
        return is_alive(_handle) ? &*slots.at(_handle.index).value : nullptr;
    }

    /**
     * @brief 可写版本，转发到 const 重载后去掉 const。
     * @param _handle 目标句柄。
     * @return 存活槽的可写指针，判死规则与 const 版完全一致。
     * @note 有效期同样到下一次 insert / erase；写入实体后由持有者负责置脏。
     */
    [[nodiscard]] T* get(handle_type _handle) noexcept { return const_cast<T*>(std::as_const(*this).get(_handle)); }

    /**
     * @brief 只判活、不取指针：下标在范围内、槽被占用且世代匹配。
     * @param _handle 目标句柄。
     * @return 句柄是否指向当前这一代对象。
     */
    [[nodiscard]] bool is_alive(handle_type _handle) const noexcept
    {
        return _handle.index < slots.size() && slots.at(_handle.index).value.has_value()
               && slots.at(_handle.index).generation == _handle.generation;
    }

    /**
     * @brief 存活实体数量。
     * @return live_count；不等于 slots.size()，空闲槽不计入。
     */
    [[nodiscard]] size_t live_size() const noexcept { return live_count; }

    /**
     * @brief 判断表里是否没有任何存活实体。
     * @return live_count 为 0 时为 true。
     */
    [[nodiscard]] bool empty() const noexcept { return live_count == 0; }

    /**
     * @brief 遍历所有存活槽的只读视图。
     * @return 惰性视图，元素为 live_reference<const T>，按槽位下标升序。
     * @note 视图引用内部 slots，有效期到下一次 insert / erase；不要在遍历中改表结构。
     */
    [[nodiscard]] auto live() const noexcept
    {
        return slots | std::views::enumerate
               | std::views::filter([](const auto& _pair) { return std::get<1>(_pair).value.has_value(); })
               | std::views::transform([](const auto& _pair) -> live_reference<const T> {
                     const auto& [index, slot] = _pair;
                     return {handle_type{.index = static_cast<uint32_t>(index), .generation = slot.generation}, *slot.value};
                 });
    }

    /**
     * @brief 可写遍历视图：把 const 视图的元素 const_cast 回可写引用。
     * @return 元素为 live_reference<T> 的惰性视图，顺序与 const 版相同。
     * @note 与 const 版同一有效期；修改实体内容不会改变句柄或槽结构。
     */
    [[nodiscard]] auto live() noexcept
    {
        return std::as_const(*this).live() | std::views::transform([](const live_reference<const T>& _entry) {
                   return live_reference<T>{_entry.handle, const_cast<T&>(_entry.value)};
               });
    }

private:
    /**
     * @brief 单个槽：实体可选值与当前世代。
     * @details optional 为空表示槽空闲；generation 在每次 erase 后自增，新槽从 0 开始。
     */
    struct slot
    {
        std::optional<T> value;  ///< @brief 槽内实体；空表示该槽未被占用
        uint32_t         generation = 0;  ///< @brief 当前世代，erase 时自增，用于让旧句柄失效
    };

    std::vector<slot>     slots;  ///< @brief 按分配顺序存放的槽数组，下标即句柄的 index
    std::vector<uint32_t> free_slots;  ///< @brief 空闲槽下标栈，insert 取末尾、erase 压入；容量在 insert 时预留，保证 erase 不分配
    size_t                live_count = 0;  ///< @brief 存活实体数，等于 slots 中 optional 非空的槽数
};
