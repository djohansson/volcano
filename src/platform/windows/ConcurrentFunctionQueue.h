#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>

#include <ppl.h>
#include <concurrent_queue.h>

class ConcurrentFunctionQueue
{
public:

	ConcurrentFunctionQueue()
		: queue()
	{ }

	template<typename _Callable, typename... _Args>
	void Queue(_Callable&& __f, _Args&&... __args)
	{
		std::function<void()> func = std::bind(std::forward<_Callable>(__f), std::forward<_Args>(__args)...);
		queue.push(func);
	}

	void RunOne()
	{
		std::function<void()> func;
		if (queue.try_pop(func))
			func();
	}

	void RunAll()
	{
		std::function<void()> func;
		while (queue.try_pop(func))
			func();
	}

	void WaitIdle(std::condition_variable& cv)
	{
		while (!queue.empty())
		{
			cv.notify_all();
			std::this_thread::yield();
		}
	}

private:

	concurrency::concurrent_queue<std::function<void()>> queue;
};


