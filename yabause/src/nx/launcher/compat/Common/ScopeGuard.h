// Stand-in for Dolphin's Common/ScopeGuard.h
#pragma once

#include <functional>
#include <optional>
#include <utility>

namespace Common
{
class ScopeGuard final
{
public:
  template <class Callable>
  ScopeGuard(Callable&& finalizer) : m_finalizer(std::forward<Callable>(finalizer))
  {
  }

  ScopeGuard(ScopeGuard&& other) : m_finalizer(std::move(other.m_finalizer))
  {
    other.m_finalizer = std::nullopt;
  }

  ~ScopeGuard() { Exit(); }

  void Dismiss() { m_finalizer.reset(); }

  void Exit()
  {
    if (m_finalizer)
    {
      (*m_finalizer)();
      Dismiss();
    }
  }

  ScopeGuard(const ScopeGuard&) = delete;
  void operator=(const ScopeGuard&) = delete;

private:
  std::optional<std::function<void()>> m_finalizer;
};
}  // namespace Common
