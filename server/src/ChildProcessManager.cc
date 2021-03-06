/*
 * Copyright (C) 2014 Project Hatohol
 *
 * This file is part of Hatohol.
 *
 * Hatohol is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License, version 3
 * as published by the Free Software Foundation.
 *
 * Hatohol is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Hatohol. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <map>
#include <semaphore.h>
#include <errno.h>
#include <Logger.h>
#include <AtomicValue.h>
#include "ChildProcessManager.h"
#include "HatoholException.h"
#include "Reaper.h"
#include "EventSemaphore.h"

using namespace std;
using namespace mlpl;

struct ChildInfo {
	const pid_t pid;
	ChildProcessManager::EventCallback * const eventCb;
	AtomicValue<bool> killed;

	ChildInfo(pid_t _pid, ChildProcessManager::EventCallback *_eventCb)
	: pid(_pid),
	  eventCb(_eventCb),
	  killed(false)
	{
		if (eventCb)
			eventCb->ref();
	}

	virtual ~ChildInfo()
	{
		if (eventCb)
			eventCb->unref();
	}

	void sendKill(void)
	{
		if (killed)
			return;
		int ret = kill(pid, SIGKILL);
		if (ret == -1 && errno == ESRCH) {
			MLPL_INFO("No process w/ pid: %d\n", pid);
			return;
		}
		HATOHOL_ASSERT(
		  ret == 0, "Failed to send kill (%d): %d\n",
		  pid, errno);
		killed = true;
	}
};

typedef map<pid_t, ChildInfo *>  ChildMap;
typedef ChildMap::iterator       ChildMapIterator;
typedef ChildMap::const_iterator ChildMapConstIterator;

struct ChildProcessManager::Impl {
	static ChildProcessManager *instance;
	static ReadWriteLock        instanceLock;

	AtomicValue<bool> resetRequest;
	EventSemaphore    resetSem;

	ReadWriteLock childrenMapLock;
	ChildMap      childrenMap;

	Impl(void)
	: resetRequest(false),
	  resetSem(0)
	{
		HATOHOL_ASSERT(sem_init(&waitChildSem, 0, 0) == 0,
		               "Failed to call sem_init(): %d\n", errno);
	}

	virtual ~Impl(void)
	{
		// TODO: wait all children
	}

	void postWaitChildSem(void)
	{
		while (true) {
			if (sem_post(&waitChildSem) == 0)
				break;
			if (errno == EINTR)
				continue;
			HATOHOL_ASSERT(true, "sem_post() failed: %d", errno);
		}
	}

	void waitWaitChildSem(void)
	{
		while (true) {
			if (sem_wait(&waitChildSem) == 0)
				break;
			if (errno == EINTR)
				continue;
			HATOHOL_ASSERT(true, "sem_wait() failed: %d", errno);
		}
	}

	void resetOnCollectThread(void)
	{
		childrenMapLock.writeLock();
		while (!childrenMap.empty()) {
			ChildMapIterator childInfoItr = childrenMap.begin();
			ChildInfo *childInfo = childInfoItr->second;
			childrenMap.erase(childInfoItr);
			// We send SIGKILL again, because new children
			// may be addded in the map after reset() is called.
			childInfo->sendKill();
			if (childInfo->eventCb)
				childInfo->eventCb->onReset();
			delete childInfo;
		}
		childrenMapLock.unlock();
		resetRequest = false;
		int err = resetSem.post();
		HATOHOL_ASSERT(err == 0,
		               "Failed to call resetSem.post(): %d\n", err);
	}

private:
	sem_t         waitChildSem;
};

ChildProcessManager *ChildProcessManager::Impl::instance = NULL;
ReadWriteLock        ChildProcessManager::Impl::instanceLock;

// ---------------------------------------------------------------------------
// EventCallback
// ---------------------------------------------------------------------------
ChildProcessManager::EventCallback::~EventCallback()
{
}

void ChildProcessManager::EventCallback::onExecuted(const bool &succeeded,
                                                    GError *gerror)
{
}

void ChildProcessManager::EventCallback::onCollected(const siginfo_t *siginfo)
{
}

void ChildProcessManager::EventCallback::onFinalized(void)
{
}

void ChildProcessManager::EventCallback::onReset(void)
{
}

// ---------------------------------------------------------------------------
// CreateArg
// ---------------------------------------------------------------------------
ChildProcessManager::CreateArg::CreateArg(void)
: flags((GSpawnFlags)G_SPAWN_DO_NOT_REAP_CHILD),
  eventCb(NULL),
  pid(0)
{
}

ChildProcessManager::CreateArg::~CreateArg()
{
	if (eventCb)
		eventCb->unref();
}

void ChildProcessManager::CreateArg::addFlag(const GSpawnFlags &flag)
{
	flags = (GSpawnFlags)(flags | flag);
}

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------
ChildProcessManager *ChildProcessManager::getInstance(void)
{
	Impl::instanceLock.readLock();
	bool hasInstance = Impl::instance;
	Impl::instanceLock.unlock();
	if (hasInstance)
		return Impl::instance;

	Impl::instanceLock.writeLock();
	if (!Impl::instance) {
		Impl::instance = new ChildProcessManager();
		Impl::instance->start();
	}
	Impl::instanceLock.unlock();
	return Impl::instance;
}

void ChildProcessManager::reset(void)
{
	struct ResetContext {
		EventSemaphore &sem;
		GMainLoop *loop;

		ResetContext (EventSemaphore &_sem)
		: sem(_sem),
		  loop(NULL)
		{
			loop = g_main_loop_new(NULL, TRUE);
			Utils::watchFdInGLibMainLoop(
			  sem.getEventFd(), G_IO_IN|G_IO_HUP|G_IO_ERR,
			  onSemaphoreReady, this);
		}

		virtual ~ResetContext()
		{
			if (loop)
				g_main_loop_unref(loop);
		}

		static gboolean onSemaphoreReady(gpointer data)
		{
			ResetContext *obj = static_cast<ResetContext *>(data);
			g_main_loop_quit(obj->loop);
			int err = obj->sem.wait();
			HATOHOL_ASSERT(
			  err == 0, "Failed to resetSem.wait(): %d", err);
			return G_SOURCE_REMOVE;
		}
	};

	m_impl->resetRequest = true;
	m_impl->postWaitChildSem();

	// We send SIGKILL to unblock waitid().
	m_impl->childrenMapLock.readLock();
	ChildMapIterator childInfoItr = m_impl->childrenMap.begin();
	for (; childInfoItr != m_impl->childrenMap.end(); ++childInfoItr) {
		ChildInfo *childInfo = childInfoItr->second;
		childInfo->sendKill();
	}
	m_impl->childrenMapLock.unlock();

	// Wait for completion of reset.
	ResetContext resetCtx(m_impl->resetSem);
	g_main_loop_run(resetCtx.loop);
}

HatoholError ChildProcessManager::create(CreateArg &arg)
{
	const gchar *workingDir =
	  arg.workingDirectory.empty() ? NULL : arg.workingDirectory.c_str();
	const GSpawnChildSetupFunc childSetup = NULL;
	const gpointer userData = NULL;
	GError *error = NULL;

	// argv
	const size_t numArgs = arg.args.size();
	if (numArgs == 0)
		return HTERR_INVALID_ARGS;
	const gchar *argv[numArgs+1];
	for (size_t i = 0; i < numArgs; i++)
		argv[i] = arg.args[i].c_str();
	argv[numArgs] = NULL;

	// envp
	const size_t numEnv = arg.envs.size();
	const gchar *envp[numEnv+1];
	for (size_t i = 0; i < numEnv; i++)
		envp[i] = arg.envs[i].c_str();
	envp[numEnv] = NULL;

	m_impl->childrenMapLock.writeLock();
	gboolean succeeded =
	  g_spawn_async(workingDir, (gchar **)argv,
	                arg.envs.empty() ? NULL : (gchar **)envp,
	                arg.flags, childSetup, userData, &arg.pid, &error);
	if (arg.eventCb)
		arg.eventCb->onExecuted(succeeded, error);
	if (!succeeded) {
		m_impl->childrenMapLock.unlock();
		string reason = "<Unknown reason>";
		if (error) {
			reason = error->message;
			g_error_free(error);
		}
		MLPL_ERR("Failed to create process: (%s), %s\n",
		         argv[0], reason.c_str());
		return HatoholError(HTERR_FAILED_TO_SPAWN, reason);
	}

	ChildInfo *childInfo = new ChildInfo(arg.pid, arg.eventCb);

	pair<ChildMapIterator, bool> result =
	  m_impl->childrenMap.insert(pair<
	    pid_t, ChildInfo *>(arg.pid, childInfo));
	m_impl->childrenMapLock.unlock();
	if (!result.second) {
		// TODO: Recovery
		HATOHOL_ASSERT(true,
		  "The previous data might still remain: %d\n", arg.pid);
	}
	m_impl->postWaitChildSem();

	return HTERR_OK;
}

// ---------------------------------------------------------------------------
// Protected methods
// ---------------------------------------------------------------------------
ChildProcessManager::ChildProcessManager(void)
: m_impl(new Impl())
{
}

ChildProcessManager::~ChildProcessManager()
{
}

gpointer ChildProcessManager::mainThread(HatoholThreadArg *arg)
{
	while (true) {
		m_impl->waitWaitChildSem();
		if (m_impl->resetRequest) {
			m_impl->resetOnCollectThread();
			continue;
		}

		siginfo_t siginfo;
		int ret = 0;
		while (true) {
			ret = waitid(P_ALL, 0, &siginfo, WEXITED);
			if (ret == -1 && errno == EINTR)
				continue;
			break;
		}
		if (ret == -1) {
			MLPL_ERR("Failed to call wait_id: %d\n", errno);
			continue;
		}
		collected(&siginfo);
	}
	return NULL;
}

bool ChildProcessManager::isDead(const siginfo_t *siginfo)
{
	switch (siginfo->si_code) {
	case CLD_EXITED:
	case CLD_KILLED:
	case CLD_DUMPED:
		return true;
	}
	return false;
}

void ChildProcessManager::collected(const siginfo_t *siginfo)
{
	ChildInfo *childInfo = NULL;

	// We use Reaper, because an exectpion might happen in onCollcted().
	Reaper<ReadWriteLock> unlocker(
	  &m_impl->childrenMapLock, ReadWriteLock::unlock);
	m_impl->childrenMapLock.writeLock();

	ChildMapIterator it = m_impl->childrenMap.find(siginfo->si_pid);
	if (it != m_impl->childrenMap.end())
		childInfo = it->second;
	if (!childInfo) {
		unlocker.reap();
		m_impl->postWaitChildSem();
		MLPL_INFO("Collected unwatched child: %d\n", siginfo->si_pid);
		return;
	}
	if (childInfo->killed)
		return; // cleanup will be in resetOnCollectThread()

	if (childInfo->eventCb) {
		if (!isDead(siginfo)) { // Ex. SIGSTOP
			m_impl->postWaitChildSem();
			return;
		}
		childInfo->eventCb->onCollected(siginfo);
	}
	m_impl->childrenMap.erase(it);
	unlocker.reap();
	if (childInfo->eventCb)
		childInfo->eventCb->onFinalized();
	delete childInfo;
}
