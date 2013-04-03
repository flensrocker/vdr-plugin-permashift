/*
 * permashift.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <vdr/plugin.h>
#include <vdr/status.h>
#include <vdr/menu.h>
#include <vdr/timers.h>
#include <vdr/shutdown.h>
#include <vdr/interface.h>

#define EXPIRECANCELPROMPT    300 // seconds to wait in user prompt before expiring recording

static const char *VERSION        = "0.5.3";
static const char *DESCRIPTION    = trNOOP("Automatically record live TV");

static const char *MenuEntry_EnablePlugin = "EnablePlugin";
static const char *MenuEntry_MaxLength = "MaxTimeshiftLength";


bool g_enablePlugin = true;
int g_maxLength = 3;


class cPluginPermashift;

// menu

class cMenuSetupLR : public cMenuSetupPage 
{
private:
	int newEnablePlugin;
	int newMaxLength;

protected:
	virtual void Store(void);

public:
	cMenuSetupLR();
};


// Status

class LRStatusMonitor : public cStatus
{
private:
	cPluginPermashift* m_plugin;

public:
	LRStatusMonitor(cPluginPermashift* plugin)
	{
		m_plugin = plugin;
	}

protected:

	virtual void ChannelSwitch(const cDevice *device, int channelNumber, bool liveView);

	virtual void TimerChange(const cTimer *Timer, eTimerChange Change);

	virtual void Recording(const cDevice *Device, const char *Name, const char *FileName, bool On);

};


class cPluginPermashift : public cPlugin
{
private:
	// our status monitor
	LRStatusMonitor *m_statusMonitor;
	// the timer we created for live recording
	cTimer* m_liveTimer;
	// store file name used for our recording for timeout recognition
	const char* m_fileName;
	// we're just starting a recording (needed for callbacks)
	bool m_startingRecording;
	// we're just stopping a recording (needed for callbacks)
	bool m_stoppingRecording;

	int m_mainThreadCounter;

public:
	cPluginPermashift(void);
	virtual ~cPluginPermashift();

	// start a recording
	bool StartLiveRecording(int channelNumber);

	// stop a recording
	bool StopLiveRecording(void);

	// status callbacks
	void ChannelSwitch(const cDevice *device, int channelNumber, bool liveView);
	void TimerChange(const cTimer *Timer, eTimerChange Change);
	void Recording(const cDevice *Device, const char *Name, const char *FileName, bool On);

	// Option: enabling plugin
	void SetEnable(bool enable) { g_enablePlugin = enable; };
	bool IsEnabled(void) { return g_enablePlugin; };

	// overrides
	virtual bool Start(void);
	virtual void Stop(void);
	virtual void MainThreadHook(void);
	virtual const char *Version(void) { return VERSION; }
	virtual const char *Description(void) { return tr(DESCRIPTION); }
	virtual const char *CommandLineHelp(void);
	virtual cMenuSetupPage *SetupMenu(void);
	virtual bool SetupParse(const char *Name, const char *Value);
};

cPluginPermashift::cPluginPermashift(void) : 
	m_statusMonitor(NULL), m_liveTimer(NULL), m_fileName(NULL),
	m_startingRecording(false), m_stoppingRecording(false),
	m_mainThreadCounter(0)
{
	g_enablePlugin = true;
	g_maxLength = Setup.InstantRecordTime / 60;
}

cPluginPermashift::~cPluginPermashift()
{
	delete m_fileName;
	delete m_statusMonitor;
}

bool cPluginPermashift::Start(void)
{
	m_statusMonitor = new LRStatusMonitor(this);
	return true;
}

void cPluginPermashift::Stop(void)
{
	// stop last recording
	StopLiveRecording();
	
	// we probably deleted a timer, so we save the Timers
	// (which isn't done by the main program after this point)
	Timers.Save();
}

void cPluginPermashift::MainThreadHook(void)
{
	// This hook is supposed to be called about once a second,
	// so let's do our checks about once a minute.
	if (m_mainThreadCounter++ >= 60)
	{
		if (m_liveTimer != NULL)
		{
			if (ShutdownHandler.IsUserInactive())
			{
				if (Interface->Confirm(tr("Press key to continue permanent timeshift"), EXPIRECANCELPROMPT, true))
				{
					StopLiveRecording();
				}
			}
		}
		m_mainThreadCounter = 0;
	}
}

void cPluginPermashift::ChannelSwitch(const cDevice *device, int channelNumber, bool liveView)
{
	if (liveView)
	{
		if (channelNumber > 0)
		{
			StartLiveRecording(channelNumber);
		}
		else
		{
			StopLiveRecording();
		}
	}
}

bool cPluginPermashift::StartLiveRecording(int channelNumber)
{
	if (!g_enablePlugin) return true;

	cChannel *channel = Channels.GetByNumber(channelNumber);
	if (channel == NULL)
	{
		esyslog("Permashift: Did not find channel!");
		return false;
	}

	// Start recording
	m_startingRecording = true;
	cRecordControls::Start(NULL, true);
	m_startingRecording = false;

	return true;
}

bool cPluginPermashift::StopLiveRecording()
{
	if (!g_enablePlugin) return true;

	if (m_liveTimer == NULL)
	{
		return true;
	}

	// First check if our pointer is still valid.
	// This should always be the case.
	bool isValid = false;
	for (cTimer* ti = Timers.First(); ti != NULL; ti = Timers.Next(ti))
	{
		if (ti == m_liveTimer)
		{
			isValid = true;
			break;
		}
	}
	if (!isValid)
	{
		esyslog("Permashift: Plugin's timer is gone!");
		m_liveTimer = NULL;
		return false;
	}

	// Check if it has been promoted and thus shouldn't be deleted by us.
	// We are setting TRANSFERPRIORITY - 1, but we delete our own recordings up to PausePriority.
	if (m_liveTimer->Priority() > Setup.PausePriority || m_liveTimer->Lifetime() > Setup.PauseLifetime)
	{
		m_liveTimer = NULL;
		return true;
	}

	// get the file name from the recorder
	cRecordControl* liveRecord = cRecordControls::GetRecordControl(m_liveTimer);
	char* fileName = liveRecord? strdup(liveRecord->FileName()) : /* m_fileName? */ NULL;
	// delete the recording and its file
	if (!fileName)
	{
		esyslog("Permashift: Did not have file name of recording to delete!");
	}

	// we're going to stop & delete
	m_stoppingRecording = true;

	// mark the timer to be stopped
	m_liveTimer->Skip();

	// process, so the recording is actually stopped
	cRecordControls::Process(time(NULL));

	// delete the timer
	Timers.Del(m_liveTimer);
	Timers.SetModified();

	// delete the recording and its file
	if (fileName)
	{
		cRecording *recording = Recordings.GetByName(fileName);
		if (recording)
		{
			if (recording->Delete())
			{
				Recordings.DelByName(fileName);
			}
			else
			{
				esyslog("Permashift: Deleting recording failed!");
			}
		}
		else
		{
			esyslog("Permashift: Did not find recording to delete!");
		}
	}

	m_stoppingRecording = false;

	m_liveTimer = NULL;
	delete fileName;

	return true;
}

void cPluginPermashift::TimerChange(const cTimer *Timer, eTimerChange Change)
{
	if (Timer == NULL) return;

	if (Change == tcAdd)
	{
		// fetch timer of our recording
		if (m_startingRecording)
		{
			// I know it'as ugly, but we need a non-const timer...
			m_liveTimer = const_cast<cTimer*>(Timer);
			// let's have a low priority, so we're not getting into the way of anyone claiming the receiver
			m_liveTimer->SetPriority(TRANSFERPRIORITY - 1);
			// adapt stop time
		    int start = m_liveTimer->Start();
		    start = start / 100 * 60 + start % 100;
			int newLength = g_maxLength * 60;
			int newStopTime = start + newLength;
			newStopTime = (newStopTime / 60) * 100 + (newStopTime % 60);
			if (newStopTime >= 2400) newStopTime -= 2400;
			m_liveTimer->SetStop(newStopTime);
		}
	}
	else if (Change == tcDel)
	{
		// when our timer is deleted from outside, delete the file as well
		if (!m_stoppingRecording && Timer == m_liveTimer)
		{
			if (Timer->IsSingleEvent() && !Timer->Recording() && Timer->StopTime() <= time(NULL))
			{
				cRecording *recording = Recordings.GetByName(m_fileName);
				if (recording)
				{
					if (recording->Delete())
					{
						Recordings.DelByName(m_fileName);
					}
				}
			}
			m_liveTimer = NULL;
		}
	}
}

void cPluginPermashift::Recording(const cDevice *Device, const char *Name, const char *FileName, bool On)
{
	// fetch the file name of our recording
	if (On)
	{
		if (m_startingRecording)
		{
			delete m_fileName;
			m_fileName = strdup(FileName);
		}
	}
}

const char *cPluginPermashift::CommandLineHelp(void)
{
	// Return a string that describes all known command line options.
	return NULL;
}

cMenuSetupPage *cPluginPermashift::SetupMenu(void)
{
	return new cMenuSetupLR();
}

bool cPluginPermashift::SetupParse(const char *Name, const char *Value)
{
	if (!strcmp(Name, MenuEntry_EnablePlugin))
	{
		g_enablePlugin = (0 == strcmp(Value, "1"));
		return true;
	}
	if (!strcmp(Name, MenuEntry_MaxLength))
	{
		g_maxLength = atoi(Value);
		return true;
	}
	return false;
}


cMenuSetupLR::cMenuSetupLR()
{
	newEnablePlugin = g_enablePlugin;
	newMaxLength = g_maxLength;
	Add(new cMenuEditBoolItem(tr("Enable plugin"), &newEnablePlugin));
	Add(new cMenuEditIntItem(tr("Maximum recording length (hours)"), &newMaxLength, 1, 23));
}

void cMenuSetupLR::Store(void)
{
	g_enablePlugin = newEnablePlugin;
	g_maxLength = newMaxLength;
	SetupStore(MenuEntry_EnablePlugin, newEnablePlugin);
	SetupStore(MenuEntry_MaxLength, newMaxLength);
}


void LRStatusMonitor::ChannelSwitch(const cDevice *device, int channelNumber, bool liveView)
{
	m_plugin->ChannelSwitch(device, channelNumber, liveView);
}

void LRStatusMonitor::TimerChange(const cTimer *Timer, eTimerChange Change)
{
	m_plugin->TimerChange(Timer, Change);
}

void LRStatusMonitor::Recording(const cDevice *Device, const char *Name, const char *FileName, bool On)
{
	m_plugin->Recording(Device, Name, FileName, On);
}

VDRPLUGINCREATOR(cPluginPermashift); // Don't touch this!
