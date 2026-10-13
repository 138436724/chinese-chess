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

template <typename T, typename H>
    requires(std::move_constructible<T>)
class scene_handle_map
{
public:
    using handle_type = scene_handle<H>;

    template <typename U>
    struct live_reference
    {
        handle_type handle;
        U&          value;
    };

    [[nodiscard]] handle_type insert(T&& _value)
    {
        if (!free_slots.empty())
        {
            const uint32_t index = free_slots.back();
            free_slots.pop_back();

            auto& slot = slots.at(index);
            slot.value.emplace(std::move(_value));
            ++live_count;

            return handle_type{.index = index, .generation = slot.generation};
        }

        slots.push_back(slot{.value = std::optional<T>(std::move(_value)), .generation = 0});
        ++live_count;

        return handle_type{.index = static_cast<uint32_t>(slots.size() - 1), .generation = 0};
    }

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

    [[nodiscard]] const T* get(handle_type _handle) const noexcept
    {
        return is_alive(_handle) ? &*slots.at(_handle.index).value : nullptr;
    }

    [[nodiscard]] T* get(handle_type _handle) noexcept { return const_cast<T*>(std::as_const(*this).get(_handle)); }

    [[nodiscard]] bool is_alive(handle_type _handle) const noexcept
    {
        return _handle.index < slots.size() && slots.at(_handle.index).value.has_value()
               && slots.at(_handle.index).generation == _handle.generation;
    }

    [[nodiscard]] size_t live_size() const noexcept { return live_count; }

    [[nodiscard]] bool empty() const noexcept { return live_count == 0; }

    void clear() noexcept
    {
        slots.clear();
        free_slots.clear();
        live_count = 0;
    }

    [[nodiscard]] auto live() const noexcept
    {
        return slots | std::views::enumerate
               | std::views::filter([](const auto& _pair) { return std::get<1>(_pair).value.has_value(); })
               | std::views::transform([](const auto& _pair) -> live_reference<const T> {
                     const auto& [index, slot] = _pair;
                     return {handle_type{.index = static_cast<uint32_t>(index), .generation = slot.generation}, *slot.value};
                 });
    }

    [[nodiscard]] auto live() noexcept
    {
        return std::as_const(*this).live() | std::views::transform([](const live_reference<const T>& _entry) {
                   return live_reference<T>{_entry.handle, const_cast<T&>(_entry.value)};
               });
    }

private:
    struct slot
    {
        std::optional<T> value;
        uint32_t         generation = 0;
    };

    std::vector<slot>     slots;
    std::vector<uint32_t> free_slots;
    size_t                live_count = 0;
};
