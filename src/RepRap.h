/****************************************************************************************************

RepRapFirmware - Reprap

RepRap is a simple class that acts as a container for an instance of all the others.

-----------------------------------------------------------------------------------------------------

Version 0.1

21 May 2013

Adrian Bowyer
RepRap Professional Ltd
http://reprappro.com

Licence: GPL

****************************************************************************************************/

#ifndef REPRAP_H
#define REPRAP_H

#include "RepRapFirmware.h"
#include "ObjectModel/ObjectModel.h"
#include "MessageType.h"
#include "RTOSIface/RTOSIface.h"

enum class ResponseSource
{
	HTTP,
	AUX,
	Generic
};

// Message box data
struct MessageBox
{
	bool active;
	String<MaxMessageLength> message;
	String<MaxTitleLength> title;
	int mode;
	uint32_t seq;
	uint32_t timer, timeout;
	AxesBitmap controls;

	MessageBox() : active(false), seq(0) { }
};

class RepRap INHERIT_OBJECT_MODEL
{
public:
	RepRap();
	void EmergencyStop();
	void Init();
	void Spin();
	void Exit();
	void Diagnostics(MessageType mtype);
	void DeferredDiagnostics(MessageType mtype) { diagnosticsDestination = mtype; }
	void Timing(MessageType mtype);

	bool Debug(Module module) const;
	void SetDebug(Module m, bool enable);
	void ClearDebug();
	void PrintDebug();
	Module GetSpinningModule() const;

	const char *GetName() const;
	void SetName(const char* nm);
	bool NoPasswordSet() const;
	bool CheckPassword(const char* pw) const;
	void SetPassword(const char* pw);

	char GetStatusCharacter() const;

	void AddTool(Tool* t);
	void DeleteTool(Tool* t);
	void SelectTool(int toolNumber, bool simulating);
	void StandbyTool(int toolNumber, bool simulating);
	Tool* GetCurrentTool() const;
	int GetCurrentToolNumber() const;
	Tool* GetTool(int toolNumber) const;
	Tool* GetCurrentOrDefaultTool() const;
	const Tool* GetFirstTool() const { return toolList; }				// Return the lowest-numbered tool
	AxesBitmap GetCurrentXAxes() const;									// Get the current axes used as X axes
	AxesBitmap GetCurrentYAxes() const;									// Get the current axes used as Y axes
	bool IsHeaterAssignedToTool(int8_t heater) const;
	unsigned int GetNumberOfContiguousTools() const;

	void AddHead(Head* head);
	void DeleteHead(Head* h);
	void SelectHead(GCodeBuffer& gb, Tool* tool, Head* head);
	Head* GetHead(int headNumber) const;
	void PrintHead(int headNumber, const StringRef& reply) const;
	void PrintHeads(const StringRef& reply) const;

	void AddPad(Pad* pad);
	void DeletePad(Pad* p);
	void SelectPad(GCodeBuffer& gb, Pad* pad);
	Pad* GetPad(int padNumber) const;
	void PrintPad(int padNumber, const StringRef& reply) const;
	void PrintPads(const StringRef& reply) const;

	Head* GetCurrentHead() const;
	int GetCurrentHeadNumber() const;
	Pad* GetCurrentPad() const;
	int GetCurrentPadNumber() const;

	void PrintCurrentHead(const StringRef& reply) const;
	void PrintCurrentPad(const StringRef& reply) const;

	void SetAccessoryInitialized(const bool initialized);
	bool GetAccessoryInitialized() const;

	void InitAccessories();

	unsigned int GetProhibitedExtruderMovements(unsigned int extrusions, unsigned int retractions);
	void PrintTool(int toolNumber, const StringRef& reply) const;
	void FlagTemperatureFault(int8_t dudHeater);
	void ClearTemperatureFault(int8_t wasDudHeater);

	void SetPowerFail(bool val);

	Platform& GetPlatform() const;
	Move& GetMove() const;
	Heat& GetHeat() const;
	GCodes& GetGCodes() const;
	Network& GetNetwork() const;
	Roland& GetRoland() const;
	Scanner& GetScanner() const;
	PrintMonitor& GetPrintMonitor() const;

#if SUPPORT_IOBITS
 	PortControl& GetPortControl() const;
#endif
#if SUPPORT_12864_LCD
 	Display& GetDisplay() const;
 	const char *GetLatestMessage(uint16_t& sequence) const;
 	const MessageBox& GetMessageBox() const { return mbox; }
#endif

	void Tick();
	bool SpinTimeoutImminent() const;
	bool IsStopped() const;
	bool IsProcessingConfig() const;

	uint16_t GetExtrudersInUse() const;
	uint16_t GetToolHeatersInUse() const;

	OutputBuffer *GetStatusResponse(uint8_t type, ResponseSource source);
	OutputBuffer *GetTrilabStatusResponse(uint8_t type, ResponseSource source);
	OutputBuffer *GetConfigResponse();
	OutputBuffer *GetLegacyStatusResponse(uint8_t type, int seq);
	OutputBuffer *GetFilesResponse(const char* dir, unsigned int startAt, bool flagsDirs, bool withTimestamp=false);
	OutputBuffer *GetSortedFilesResponse(const char* dir, unsigned int startAt, bool flagsDirs);
	OutputBuffer *GetFilelistResponse(const char* dir, unsigned int startAt);
	bool GetFileInfoResponse(const char *filename, OutputBuffer *&response, bool quitEarly);

	void Beep(unsigned int freq, unsigned int ms);
	void SetMessage(const char *msg);
	void SetAlert(const char *msg, const char *title, int mode, float timeout, AxesBitmap controls);
	void ClearAlert();

	bool WriteToolSettings(FileStore *f) const;				// save some information for the resume file
	bool WriteToolParameters(FileStore *f) const;			// save some information in config-override.g
	bool WriteAxisStepsParameters(FileStore *f) const;

	bool WriteHeadsList(FileStore *f) const;
	bool WritePadsList(FileStore *f) const;
	bool WriteSelectedHeads(FileStore *f) const;
	bool WriteSelectedPad(FileStore *f) const;
	bool WriteSelectedTool(FileStore *f) const;

	bool WriteAccessoryStatus(FileStore *f) const;

	void ReportInternalError(const char *file, const char *func, int line) const;	// Report an internal error

	static uint32_t DoDivide(uint32_t a, uint32_t b);		// helper function for diagnostic tests
	static float SinfCosf(float angle);						// helper function for diagnostic tests
	static double SinCos(double angle);						// helper function for diagnostic tests

	uint32_t GetLastAuxActivity() { return lastAuxActivity; }
	void SetLastAuxActivity();

#ifdef RTOS
	void KickHeatTaskWatchdog() { heatTaskIdleTicks = 0; }
#endif

protected:
	DECLARE_OBJECT_MODEL

private:
	static void EncodeString(StringRef& response, const char* src, size_t spaceToLeave, bool allowControlChars = false, char prefix = 0);

	static constexpr uint32_t MaxTicksInSpinState = 20000;	// timeout before we reset the processor
	static constexpr uint32_t HighTicksInSpinState = 16000;	// how long before we warn that timeout is approaching

	Platform* platform;
	Network* network;
	Move* move;
	Heat* heat;
	GCodes* gCodes;
	Roland* roland;
	Scanner* scanner;
 	PrintMonitor* printMonitor;

#if SUPPORT_IOBITS
 	PortControl *portControl;
#endif

#if SUPPORT_12864_LCD
 	Display *display;
#endif

 	Mutex toolListMutex, messageBoxMutex;
	Tool* toolList;								// the tool list is sorted in order of increasing tool number
	Tool* currentTool;

	Mutex headListMutex;
	Head* headList;

	Mutex padListMutex;
	Pad* padList;
	Pad* currentPad;

	uint32_t lastWarningMillis;					// When we last sent a warning message for things that can happen very often

	//uint32_t lastSendStatus;
	uint32_t lastCheckSafetyTimer;
	//uint32_t lastSendDiagnostics;
	uint32_t lastAuxActivity;
	uint32_t lastAuxActivityCheck;

	uint16_t activeExtruders;
	uint16_t activeToolHeaters;

	uint16_t ticksInSpinState;
#ifdef RTOS
	uint16_t heatTaskIdleTicks;
#endif
	Module spinningModule;
	uint32_t fastLoop, slowLoop;

	uint32_t debug;
	bool stopped;
	bool active;
	bool resetting;
	bool processingConfig;
	bool powerFail;

	String<RepRapPasswordLength> password;
	String<MachineNameLength> myName;

	unsigned int beepFrequency, beepDuration;
	String<MaxMessageLength> message;
	uint16_t messageSequence;

	MessageBox mbox;					// message box data

	// Deferred diagnostics
	MessageType diagnosticsDestination;
	bool accessoryInitialized;
	bool justSentDiagnostics;
};

inline Platform& RepRap::GetPlatform() const { return *platform; }
inline Move& RepRap::GetMove() const { return *move; }
inline Heat& RepRap::GetHeat() const { return *heat; }
inline GCodes& RepRap::GetGCodes() const { return *gCodes; }
inline Network& RepRap::GetNetwork() const { return *network; }
inline Roland& RepRap::GetRoland() const { return *roland; }
inline Scanner& RepRap::GetScanner() const { return *scanner; }
inline PrintMonitor& RepRap::GetPrintMonitor() const { return *printMonitor; }

#if SUPPORT_IOBITS
inline PortControl& RepRap::GetPortControl() const { return *portControl; }
#endif

#if SUPPORT_12864_LCD
inline Display& RepRap::GetDisplay() const { return *display; }
#endif

inline bool RepRap::Debug(Module m) const { return debug & (1 << m); }
inline Module RepRap::GetSpinningModule() const { return spinningModule; }

inline Tool* RepRap::GetCurrentTool() const { return currentTool; }
inline uint16_t RepRap::GetExtrudersInUse() const { return activeExtruders; }
inline uint16_t RepRap::GetToolHeatersInUse() const { return activeToolHeaters; }
inline bool RepRap::IsStopped() const { return stopped; }
inline bool RepRap::IsProcessingConfig() const { return processingConfig; }

inline Pad* RepRap::GetCurrentPad() const { return currentPad; }

inline void RepRap::SetPowerFail(bool val) { powerFail = val; }

#define INTERNAL_ERROR do { reprap.ReportInternalError((__FILE__), (__func__), (__LINE__)); } while(0)

#endif


