/*
 * LinuxInterface.cpp
 *
 *  Created on: 29 Mar 2019
 *      Author: Christian
 */

#include "LinuxInterface.h"
#include "DataTransfer.h"

#if HAS_LINUX_INTERFACE

#include <GCodes/GCodeBuffer/ExpressionParser.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>
#include <Heating/Heat.h>
#include <Movement/Move.h>
#include <Platform/Platform.h>
#include <PrintMonitor/PrintMonitor.h>
#include <Tools/Filament.h>
#include <Platform/RepRap.h>
#include <RepRapFirmware.h>
#include <Platform/Tasks.h>
#include <Hardware/SoftwareReset.h>
#include <Hardware/ExceptionHandlers.h>
#include <Platform/TaskPriorities.h>

extern char _estack;		// defined by the linker

volatile OutputStack LinuxInterface::gcodeReply;
Mutex LinuxInterface::gcodeReplyMutex;

// The SBC stack size needs to be enough to support rr_model and expression evaluation
// In RRF 3.3beta3, 744 is only just enough for simple expression evaluation in a release build when using globals
// In 3.3beta3.1 we have saved ~151 bytes (37 words) of stack compared to 3.3beta3
#ifdef __LPC17xx__
constexpr size_t SBCTaskStackWords = 375;
#elif defined(DEBUG)
constexpr size_t SBCTaskStackWords = 1200;			// debug builds use more stack
#else
constexpr size_t SBCTaskStackWords = 1000;			// increased from 820 so that we can evaluate "abs(move.calibration.initial.deviation - move.calibration.final.deviation) < 0.000"
#endif

constexpr uint32_t LinuxYieldTimeout = 10;

static Task<SBCTaskStackWords> *sbcTask;

extern "C" [[noreturn]] void SBCTaskStart(void * pvParameters) noexcept
{
	reprap.GetLinuxInterface().TaskLoop();
}

LinuxInterface::LinuxInterface() noexcept : isConnected(false), numDisconnects(0), numTimeouts(0),
	maxDelayBetweenTransfers(SpiTransferDelay), maxFileOpenDelay(SpiFileOpenDelay), numMaxEvents(SpiEventsRequired),
	delaying(false), numEvents(0), reportPause(false), reportPauseWritten(false), printAborted(false),
	codeBuffer(nullptr), rxPointer(0), txPointer(0), txEnd(0), sendBufferUpdate(true), writingIap(false), iapWritePointer(IAP_IMAGE_START),
	waitingForFileChunk(false), fileMutex(), fileSemaphore(), fileOperation(FileOperation::none), fileOperationPending(false)
#ifdef TRACK_FILE_CODES
	, fileCodesRead(0), fileCodesHandled(0), fileMacrosRunning(0), fileMacrosClosing(0)
#endif
{
}

void LinuxInterface::Init() noexcept
{
	if (reprap.UsingLinuxInterface())
	{
		fileMutex.Create("SBCFile");
		gcodeReplyMutex.Create("SBCReply");
		codeBuffer = (char *)new uint32_t[(SpiCodeBufferSize + 3)/4];

#if defined(DUET_NG)
		// Make sure that the Wifi module if present is disabled. The ESP Reset pin is already forced low in Platform::Init();
		pinMode(EspEnablePin, OUTPUT_LOW);
#endif

		transfer.Init();
		sbcTask = new Task<SBCTaskStackWords>;
		sbcTask->Create(SBCTaskStart, "SBC", nullptr, TaskPriority::SbcPriority);
		iapRamAvailable = &_estack - Tasks::GetHeapTop();
	}
	else
	{
		// Set up the data transfer to exchange the header + response code. No task is started to save memory
		transfer.Init();
	}
}

void LinuxInterface::Spin() noexcept
{
	if (transfer.IsReady())
	{
		// Don't process anything, just kick off the next transfer to report we're operating in standalone mode
		transfer.StartNextTransfer();
	}
}

[[noreturn]] void LinuxInterface::TaskLoop() noexcept
{
	transfer.InitFromTask();
	transfer.StartNextTransfer();

	bool isReady = false, hadReset = false, skipNextDelay = false;
	for (;;)
	{
		if (hadReset)
		{
			isReady = true;
			hadReset = false;
		}
		else if (transfer.IsReady())
		{
			isReady = true;
			hadReset = isConnected && transfer.LinuxHadReset();
		}
		else
		{
			isReady = false;
		}

		if (isReady && !hadReset)
		{
			// Check if the connection state has changed
			if (!isConnected && !writingIap)
			{
				isConnected = true;
				reprap.GetPlatform().Message(NetworkInfoMessage, "Connection to Linux established!\n");
			}

			// Process incoming packets
			bool codeBufferAvailable = true;
			for (size_t i = 0; i < transfer.PacketsToRead(); i++)
			{
				const PacketHeader * const packet = transfer.ReadPacket();
				if (packet == nullptr)
				{
					if (reprap.Debug(moduleLinuxInterface))
					{
						debugPrintf("Error trying to read next SPI packet\n");
					}
					break;
				}

				if (packet->request >= (uint16_t)LinuxRequest::InvalidRequest)
				{
					REPORT_INTERNAL_ERROR;
					break;
				}

				bool packetAcknowledged = true;
				switch ((LinuxRequest)packet->request)
				{
				// Perform an emergency stop
				case LinuxRequest::EmergencyStop:
					reprap.EmergencyStop();
					break;

				// Reset the controller
				case LinuxRequest::Reset:
					SoftwareReset(SoftwareResetReason::user);
					break;

				// Perform a G/M/T-code
				case LinuxRequest::Code:
				{
					// Read the next code
					if (packet->length == 0)
					{
						reprap.GetPlatform().Message(WarningMessage, "Received empty binary code, discarding\n");
						break;
					}

					const CodeHeader *code = reinterpret_cast<const CodeHeader*>(transfer.ReadData(packet->length));
					const GCodeChannel channel(code->channel);
					GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
					if (gb->IsInvalidated())
					{
						// Don't deal with codes that will be thrown away
						break;
					}

					// Check if a GB is waiting for a macro file to be started
					if (gb->IsWaitingForMacro() && !gb->IsMacroRequestPending())
					{
						gb->ResolveMacroRequest(false, false);
#ifdef TRACK_FILE_CODES
						if (channel == GCodeChannel::File)
						{
							fileMacrosRunning++;
						}
#endif
					}

					// Don't process any more codes if we failed to store them last time...
					if (!codeBufferAvailable)
					{
						packetAcknowledged = false;
						break;
					}

					TaskCriticalSectionLocker locker;

					// Make sure no existing codes are overwritten
					uint16_t bufferedCodeSize = sizeof(BufferedCodeHeader) + packet->length;
					if ((txEnd == 0 && bufferedCodeSize > max<uint16_t>(rxPointer, SpiCodeBufferSize - txPointer)) ||
						(txEnd != 0 && bufferedCodeSize > rxPointer - txPointer))
					{
						//debugPrintf("Failed to store code, RX/TX %d/%d-%d\n", rxPointer, txPointer, txEnd);
						packetAcknowledged = codeBufferAvailable = false;
						break;
					}

					// Overlap if necessary
					if (txPointer + bufferedCodeSize > SpiCodeBufferSize)
					{
						txEnd = txPointer;
						txPointer = 0;
						sendBufferUpdate = true;
					}

					// Store the buffer header
					BufferedCodeHeader *bufHeader = reinterpret_cast<BufferedCodeHeader *>(codeBuffer + txPointer);
					bufHeader->isPending = true;
					bufHeader->length = packet->length;

					// Store the corresponding code. Binary codes are always aligned on a 4-byte boundary
					uint32_t *dst = reinterpret_cast<uint32_t *>(codeBuffer + txPointer + sizeof(BufferedCodeHeader));
					const uint32_t *src = reinterpret_cast<const uint32_t *>(code);
					memcpyu32(dst, src, packet->length / sizeof(uint32_t));
					txPointer += bufferedCodeSize;
					break;
				}

				// Get the object model
				case LinuxRequest::GetObjectModel:
				{
					String<StringLength100> key;
					String<StringLength20> flags;
					transfer.ReadGetObjectModel(packet->length, key.GetRef(), flags.GetRef());

					try
					{
						OutputBuffer *outBuf = reprap.GetModelResponse(key.c_str(), flags.c_str());
						if (outBuf == nullptr || !transfer.WriteObjectModel(outBuf))
						{
							// Failed to write the whole object model, try again later
							packetAcknowledged = false;
							OutputBuffer::ReleaseAll(outBuf);
						}
					}
					catch (const GCodeException& e)
					{
						// Get the error message and send it back to DSF
						OutputBuffer *buf;
						if (OutputBuffer::Allocate(buf))
						{
							String<StringLength100> errorMessage;
							e.GetMessage(errorMessage.GetRef(), nullptr);
							buf->cat(errorMessage.c_str());
							if (!transfer.WriteObjectModel(buf))
							{
								OutputBuffer::ReleaseAll(buf);
								packetAcknowledged = false;
							}
						}
						else
						{
							packetAcknowledged = false;
						}
					}
					break;
				}

				// Set value in the object model
				case LinuxRequest::SetObjectModel:
				{
					const size_t dataLength = packet->length;
					const char * const data = transfer.ReadData(dataLength);
					// TODO implement this
					(void)data;
					break;
				}

				// Print is about to be started, set file print info
				case LinuxRequest::SetPrintFileInfo:
				{
					String<MaxFilenameLength> filename;
					transfer.ReadPrintStartedInfo(packet->length, filename.GetRef(), fileInfo);
					reprap.GetPrintMonitor().SetPrintingFileInfo(filename.c_str(), fileInfo);
					break;
				}

				// Print has been stopped
				case LinuxRequest::PrintStopped:
				{
					const PrintStoppedReason reason = transfer.ReadPrintStoppedInfo();
					if (reason == PrintStoppedReason::abort)
					{
						// Stop the print with the given reason
						printAborted = true;
						InvalidateBufferedCodes(GCodeChannel::File);
					}
					else
					{
						// Just mark the print file as finished
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(GCodeChannel::File);
						MutexLocker locker(gb->mutex, LinuxYieldTimeout);
						if (locker.IsAcquired())
						{
							gb->SetPrintFinished();
						}
						else
						{
							packetAcknowledged = false;
						}
					}
					break;
				}

				// Macro file has been finished
				case LinuxRequest::MacroCompleted:
				{
					bool error;
					const GCodeChannel channel = transfer.ReadMacroCompleteInfo(error);
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
						if (gb->IsWaitingForMacro() && !gb->IsMacroRequestPending())
						{
							gb->ResolveMacroRequest(error, true);
							if (reprap.Debug(moduleLinuxInterface))
							{
								debugPrintf("Waiting macro completed on channel %u\n", channel.ToBaseType());
							}
						}
						else
						{
							MutexLocker locker(gb->mutex, LinuxYieldTimeout);
							if (locker.IsAcquired())
							{
								if (error)
								{
									gb->CurrentFileMachineState().CloseFile();
									gb->PopState();
									gb->Init();
								}
								else
								{
#ifdef TRACK_FILE_CODES
									if (channel == GCodeChannel::File)
									{
										fileMacrosClosing++;
									}
#endif
									gb->SetFileFinished();
								}

								if (reprap.Debug(moduleLinuxInterface))
								{
									debugPrintf("Macro completed on channel %u\n", channel.ToBaseType());
								}
							}
							else
							{
								packetAcknowledged = false;
							}
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Return heightmap as generated by G29 S0
				case LinuxRequest::GetHeightMap:
				{
					ConditionalReadLocker locker(reprap.GetMove().heightMapLock);
					if (locker.IsLocked())
					{
						packetAcknowledged = transfer.WriteHeightMap();
					}
					else
					{
						packetAcknowledged = false;
					}
					break;
				}

				// Set heightmap via G29 S1
				case LinuxRequest::SetHeightMap:
				{
					ConditionalWriteLocker locker(reprap.GetMove().heightMapLock);
					if (locker.IsLocked())
					{
						if (!transfer.ReadHeightMap())
						{
							reprap.GetPlatform().Message(ErrorMessage, "Failed to set height map - bad data?\n");
						}
					}
					else
					{
						packetAcknowledged = false;
					}
					break;
				}

				// Lock movement and wait for standstill
				case LinuxRequest::LockMovementAndWaitForStandstill:
				{
					const GCodeChannel channel = transfer.ReadCodeChannel();
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
						MutexLocker locker(gb->mutex, LinuxYieldTimeout);
						if (locker.IsAcquired() && reprap.GetGCodes().LockMovementAndWaitForStandstill(*gb))
						{
							transfer.WriteLocked(channel);
						}
						else
						{
							packetAcknowledged = false;
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Unlock everything
				case LinuxRequest::Unlock:
				{
					const GCodeChannel channel = transfer.ReadCodeChannel();
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
						MutexLocker locker(gb->mutex, LinuxYieldTimeout);
						if (locker.IsAcquired())
						{
							reprap.GetGCodes().UnlockAll(*gb);
						}
						else
						{
							packetAcknowledged = false;
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Write another chunk of the IAP binary to the designated Flash area
				case LinuxRequest::WriteIap:
				{
					if (iapWritePointer == IAP_IMAGE_START)			// if start of IAP write
					{
						reprap.PrepareToLoadIap();
						writingIap = true;
					}

					// Copy another IAP chunk. It's always bound on a 4-byte boundary
					uint32_t *dst = reinterpret_cast<uint32_t *>(iapWritePointer);
					const uint32_t *src = reinterpret_cast<const uint32_t *>(transfer.ReadData(packet->length));
					memcpyu32(dst, src, packet->length / sizeof(uint32_t));
					iapWritePointer += packet->length;
					break;
				}

				// Launch the IAP binary
				case LinuxRequest::StartIap:
					reprap.StartIap(nullptr);
					break;

				// Assign filament (deprecated)
				case LinuxRequest::AssignFilament_deprecated:
					(void)transfer.ReadData(packet->length);		// skip the packet content
					break;

				// Return a file chunk
				case LinuxRequest::FileChunk:
					transfer.ReadFileChunk(requestedFileBuffer, requestedFileDataLength, requestedFileLength);
					requestedFileSemaphore.Give();
					break;

				// Evaluate an expression
				case LinuxRequest::EvaluateExpression:
				{
					String<GCODE_LENGTH> expression;
					const GCodeChannel channel = transfer.ReadEvaluateExpression(packet->length, expression.GetRef());
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);

						// If there is a macro file waiting, the first instruction must be conditional. Don't block any longer...
						if (gb->IsWaitingForMacro())
						{
							gb->ResolveMacroRequest(false, false);
#ifdef TRACK_FILE_CODES
							if (channel == GCodeChannel::File)
							{
								fileMacrosRunning++;
							}
#endif
						}

						try
						{
							// Evaluate the expression and send the result to DSF
							MutexLocker lock(gb->mutex, LinuxYieldTimeout);
							if (lock.IsAcquired())
							{
								ExpressionParser parser(*gb, expression.c_str(), expression.c_str() + expression.strlen());
								const ExpressionValue val = parser.Parse();
								packetAcknowledged = transfer.WriteEvaluationResult(expression.c_str(), val);
							}
							else
							{
								packetAcknowledged = false;
							}
						}
						catch (const GCodeException& e)
						{
							// Get the error message and send it back to DSF
							String<StringLength100> errorMessage;
							e.GetMessage(errorMessage.GetRef(), nullptr);
							packetAcknowledged = transfer.WriteEvaluationError(expression.c_str(), errorMessage.c_str());
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Send a firmware message, typically a response to a command that has been passed to DSF.
				// These responses can get quite long (e.g. responses to M20) so receive it into an OutputBuffer.
				case LinuxRequest::Message:
				{
					OutputBuffer *buf;
					if (OutputBuffer::Allocate(buf))
					{
						MessageType type;
						if (transfer.ReadMessage(type, buf))
						{
							// FIXME Push flag is not supported yet
							reprap.GetPlatform().Message(type, buf);
						}
						else
						{
							// Not enough memory for reading the whole message, try again later
							OutputBuffer::ReleaseAll(buf);
							packetAcknowledged = false;
						}
					}
					break;
				}

				// Macro file has been started
				case LinuxRequest::MacroStarted:
				{
					const GCodeChannel channel = transfer.ReadCodeChannel();
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
						if (gb->IsWaitingForMacro() && !gb->IsMacroRequestPending())
						{
							// File exists and is open, but no code has arrived yet
							gb->ResolveMacroRequest(false, false);
#ifdef TRACK_FILE_CODES
							if (channel == GCodeChannel::File)
							{
								fileMacrosRunning++;
							}
#endif
						}
						else if (channel != GCodeChannel::Daemon)
						{
							reprap.GetPlatform().MessageF(WarningMessage, "Macro file has been started on channel %s but none was requested\n", channel.ToString());
						}
						else
						{
							// dameon.g is running, now the OM may report the file is being executed
							reprap.InputsUpdated();
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Invalidate all files and codes on a given channel
				case LinuxRequest::InvalidateChannel:
				{
					const GCodeChannel channel = transfer.ReadCodeChannel();
					if (channel.IsValid())
					{
						GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
						if (gb->IsWaitingForMacro())
						{
							gb->ResolveMacroRequest(true, false);
						}

						MutexLocker locker(gb->mutex, LinuxYieldTimeout);
						if (locker.IsAcquired())
						{
							// Note that we do not call StopPrint here or set any other variables; DSF already does that
							gb->AbortFile(true, false);
							InvalidateBufferedCodes(channel);
						}
						else
						{
							packetAcknowledged = false;
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;
				}

				// Set the content of a variable
				case LinuxRequest::SetVariable:
				{
					bool createVariable;
					String<MaxVariableNameLength> varName;
					String<GCODE_LENGTH> expression;
					const GCodeChannel channel = transfer.ReadSetVariable(createVariable, varName.GetRef(), expression.GetRef());

					// Make sure we can access the gb safely...
					if (!channel.IsValid())
					{
						REPORT_INTERNAL_ERROR;
						break;
					}

					GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
					MutexLocker lock(gb->mutex, LinuxYieldTimeout);
					if (!lock.IsAcquired())
					{
						packetAcknowledged = false;
						break;
					}

					// Get the variable set
					const bool isGlobal = StringStartsWith(varName.c_str(), "global.");
					if (!isGlobal && !StringStartsWith(varName.c_str(), "var."))
					{
						packetAcknowledged = transfer.WriteSetVariableError(varName.c_str(), "expected a global or local variable");
						break;
					}
					WriteLockedPointer<VariableSet> vset = (isGlobal) ? reprap.GetGlobalVariablesForWriting() : WriteLockedPointer<VariableSet>(nullptr, &gb->GetVariables());

					// Check if the variable is valid
					const char *shortVarName = varName.c_str() + strlen(isGlobal ? "global." : "var.");
					Variable * const v = vset->Lookup(shortVarName);
					if (createVariable && v != nullptr)
					{
						// For now we don't allow an existing variable to be reassigned using a 'var' or 'global' statement. We may need to allow it for 'global' statements.
						// Save memory by re-using 'expression' to capture the error message
						expression.printf("variable '%s' already exists", varName.c_str());
						packetAcknowledged = transfer.WriteSetVariableError(varName.c_str(), expression.c_str());
						break;
					}
					if (!createVariable && v == nullptr)
					{
						// Save memory by re-using 'expression' to capture the error message
						expression.printf("unknown variable '%s'", varName.c_str());
						packetAcknowledged = transfer.WriteSetVariableError(varName.c_str(), expression.c_str());
						break;
					}

					// Evaluate the expression and assign it
					try
					{
						ExpressionParser parser(*gb, expression.c_str(), expression.c_str() + expression.strlen());
						ExpressionValue ev = parser.Parse();
						if (v == nullptr)
						{
							// DSF doesn't provide indent values but instructs RRF to delete local variables when the current block ends
							vset->InsertNew(shortVarName, ev, 0);
						}
						else
						{
							v->Assign(ev);
						}

						transfer.WriteSetVariableResult(varName.c_str(), ev);
						if (isGlobal)
						{
							reprap.GlobalUpdated();
						}
					}
					catch (const GCodeException& e)
					{
						// Get the error message and send it back to DSF
						// Save memory by re-using 'expression' to capture the error message
						e.GetMessage(expression.GetRef(), nullptr);
						packetAcknowledged = transfer.WriteSetVariableError(varName.c_str(), expression.c_str());
					}
					break;
				}

				// Delete a local variable
				case LinuxRequest::DeleteLocalVariable:
				{
					String<MaxVariableNameLength> varName;
					const GCodeChannel channel = transfer.ReadDeleteLocalVariable(varName.GetRef());

					// Make sure we can access the gb safely...
					if (!channel.IsValid())
					{
						REPORT_INTERNAL_ERROR;
						break;
					}

					GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);
					MutexLocker lock(gb->mutex, LinuxYieldTimeout);
					if (!lock.IsAcquired())
					{
						packetAcknowledged = false;
						break;
					}

					// Try to delete the variable again
					WriteLockedPointer<VariableSet> vset = WriteLockedPointer<VariableSet>(nullptr, &gb->GetVariables());
					vset.Ptr()->Delete(varName.c_str());
					break;
				}

				// Result of a file exists check
				case LinuxRequest::CheckFileExistsResult:
					if (fileOperation == FileOperation::checkFileExists)
					{
						fileSuccess = transfer.ReadBoolean();
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;


				// Result of a deletion request
				case LinuxRequest::FileDeleteResult:
					if (fileOperation == FileOperation::deleteFileOrDirectory)
					{
						fileSuccess = transfer.ReadBoolean();
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Result of a file open request
				case LinuxRequest::OpenFileResult:
					if (fileOperation == FileOperation::openRead ||
						fileOperation == FileOperation::openWrite ||
						fileOperation == FileOperation::openAppend)
					{
						fileHandle = transfer.ReadOpenFileResult(fileOffset);
						fileSuccess = (fileHandle != noFileHandle);
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Result of a file read request
				case LinuxRequest::FileReadResult:
					if (fileOperation == FileOperation::read)
					{
						int bytesRead = transfer.ReadFileData(fileReadBuffer, fileBufferLength);
						fileSuccess = bytesRead >= 0;
						fileOffset = fileSuccess ? bytesRead : 0;
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Result of a file write request
				case LinuxRequest::FileWriteResult:
					if (fileOperation == FileOperation::write)
					{
						fileSuccess = transfer.ReadBoolean();
						if (!fileSuccess || fileBufferLength == 0)
						{
							fileOperationPending = false;
							fileOperation = FileOperation::none;
							fileSemaphore.Give();
						}
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Result of a file seek request
				case LinuxRequest::FileSeekResult:
					if (fileOperation == FileOperation::seek)
					{
						fileSuccess = transfer.ReadBoolean();
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Result of a file seek request
				case LinuxRequest::FileTruncateResult:
					if (fileOperation == FileOperation::truncate)
					{
						fileSuccess = transfer.ReadBoolean();
						fileOperation = FileOperation::none;
						fileSemaphore.Give();
					}
					else
					{
						REPORT_INTERNAL_ERROR;
					}
					break;

				// Invalid request
				default:
					REPORT_INTERNAL_ERROR;
					break;
				}

				// Request the packet again if no response could be sent back
				if (!packetAcknowledged)
				{
					transfer.ResendPacket(packet);
				}
			}

			if (!writingIap)					// it's not safe to access other resources once we have started writing the IAP
			{
				// Check if we can wait a short moment to reduce CPU load on the SBC
				if (!skipNextDelay && numEvents < numMaxEvents && !waitingForFileChunk &&
					!fileOperationPending && fileOperation == FileOperation::none)
				{
					delaying = true;
					if (!TaskBase::Take(MassStorage::AnyFileOpen() ? maxFileOpenDelay : maxDelayBetweenTransfers))
					{
						delaying = false;
					}
				}
				numEvents = 0;
				skipNextDelay = false;

				// Send code replies and generic messages
				if (!gcodeReply.IsEmpty())
				{
					MutexLocker lock(gcodeReplyMutex);
					while (!gcodeReply.IsEmpty())
					{
						const MessageType type = gcodeReply.GetFirstItemType();
						OutputBuffer *buffer = gcodeReply.GetFirstItem();			// this may be null
						if (!transfer.WriteCodeReply(type, buffer))					// this handles the null case too
						{
							break;
						}
						gcodeReply.SetFirstItem(buffer);							// this does a pop if buffer is null
					}
				}

				// Notify DSF about the available buffer space
				if (!codeBufferAvailable || sendBufferUpdate)
				{
					TaskCriticalSectionLocker locker;

					const uint16_t bufferSpace = (txEnd == 0) ? max<uint16_t>(rxPointer, SpiCodeBufferSize - txPointer) : rxPointer - txPointer;
					sendBufferUpdate = !transfer.WriteCodeBufferUpdate(bufferSpace);
				}

				// Get another chunk of the file being requested
				if (waitingForFileChunk &&
					!fileChunkRequestSent && transfer.WriteFileChunkRequest(requestedFileName.c_str(), requestedFileOffset, requestedFileLength))
				{
					fileChunkRequestSent = true;
				}

				// Perform the next file operation if requested
				if (fileOperationPending)
				{
					switch (fileOperation)
					{
					case FileOperation::checkFileExists:
						fileOperationPending = !transfer.WriteCheckFileExists(filePath);
						break;

					case FileOperation::deleteFileOrDirectory:
						fileOperationPending = !transfer.WriteDeleteFileOrDirectory(filePath);
						break;

					case FileOperation::openRead:
					case FileOperation::openWrite:
					case FileOperation::openAppend:
						fileOperationPending = !transfer.WriteOpenFile(filePath, fileOperation == FileOperation::openWrite || fileOperation == FileOperation::openAppend, fileOperation == FileOperation::openAppend, filePreAllocSize);
						break;

					case FileOperation::read:
						fileOperationPending = !transfer.WriteReadFile(fileHandle, fileBufferLength);
						break;

					case FileOperation::write:
					{
						size_t bytesNotWritten = fileBufferLength;
						if (transfer.WriteFileData(fileHandle, fileWriteBuffer, fileBufferLength))
						{
							fileWriteBuffer += bytesNotWritten - fileBufferLength;
							if (fileBufferLength == 0)
							{
								fileOperationPending = false;
							}
						}
						break;
					}

					case FileOperation::seek:
						fileOperationPending = !transfer.WriteSeekFile(fileHandle, fileOffset);
						break;

					case FileOperation::truncate:
						fileOperationPending = !transfer.WriteTruncateFile(fileHandle);
						break;

					case FileOperation::close:
						fileOperationPending = !transfer.WriteCloseFile(fileHandle);
						if (!fileOperationPending)
						{
							// Close requests don't get a result back, so they can be resolved as soon as they are sent to the SBC
							fileOperation = FileOperation::none;
							fileSemaphore.Give();
						}
						break;

					default:
						REPORT_INTERNAL_ERROR;
						break;
					}
				}

				// Deal with code channel requests
				for (size_t i = 0; i < NumGCodeChannels; i++)
				{
					const GCodeChannel channel(i);
					GCodeBuffer * const gb = reprap.GetGCodes().GetGCodeBuffer(channel);

					// Invalidate buffered codes if required
					if (gb->IsInvalidated())
					{
						InvalidateBufferedCodes(gb->GetChannel());
						gb->Invalidate(false);
					}

					// Deal with macro files being closed
					if (gb->IsMacroFileClosed() && transfer.WriteMacroFileClosed(channel))
					{
						// Note this is only sent when a macro file has finished successfully
						gb->MacroFileClosedSent();
					}

					// Handle blocking macro requests
					if (gb->IsWaitingForMacro() && gb->IsMacroRequestPending())
					{
						const char * const requestedMacroFile = gb->GetRequestedMacroFile();
						bool fromCode = gb->IsMacroStartedByCode();
						if (transfer.WriteMacroRequest(channel, requestedMacroFile, fromCode))
						{
							if (reprap.Debug(moduleLinuxInterface))
							{
								debugPrintf("Requesting macro file '%s' (fromCode: %s)\n", requestedMacroFile, fromCode ? "true" : "false");
							}
							gb->MacroRequestSent();
							gb->Invalidate();
						}
					}

					// Deal with other requests unless we are still waiting in a semaphore
					if (!gb->IsWaitingForMacro())
					{
						MutexLocker gbLock(gb->mutex, LinuxYieldTimeout);
						if (gbLock.IsAcquired())
						{
							if (gb->GetChannel() != GCodeChannel::Daemon)
							{
								skipNextDelay |= gb->IsMacroRequestPending() || gb->HasJustStartedMacro();
							}

							// Handle file abort requests
							if (gb->IsAbortRequested() && transfer.WriteAbortFileRequest(channel, gb->IsAbortAllRequested()))
							{
#ifdef TRACK_FILE_CODES
								if (channel == GCodeChannel::File)
								{
									if (gb->IsAbortAllRequested())
									{
										fileCodesRead = fileCodesHandled = fileMacrosRunning = fileMacrosClosing = 0;
									}
									else
									{
										fileMacrosClosing++;
									}
								}
#endif
								gb->FileAbortSent();
								gb->Invalidate();
							}

							// Handle blocking messages and their results
							if (gb->LatestMachineState().waitingForAcknowledgement && gb->IsMessagePromptPending() &&
								transfer.WriteWaitForAcknowledgement(channel))
							{
								gb->MessagePromptSent();
								gb->Invalidate();
							}
							else if (gb->IsMessageAcknowledged() && transfer.WriteMessageAcknowledged(channel))
							{
								// Note this is only sent when a message was acknowledged in a regular way (i.e. by M292)
								gb->MessageAcknowledgementSent();
							}

							// Handle non-blocking macro requests (e.g. daemon.g)
							if (gb->IsMacroRequestPending())
							{
								const char * const requestedMacroFile = gb->GetRequestedMacroFile();
								bool fromCode = gb->IsMacroStartedByCode();
								if (transfer.WriteMacroRequest(channel, requestedMacroFile, fromCode))
								{
									if (reprap.Debug(moduleLinuxInterface))
									{
										debugPrintf("Requesting non-blocking macro file '%s' (fromCode: %s)\n", requestedMacroFile, fromCode ? "true" : "false");
									}
									gb->MacroRequestSent();
									gb->Invalidate();
								}
							}

							// Send pending firmware codes
							if (gb->IsSendRequested() && transfer.WriteDoCode(channel, gb->DataStart(), gb->DataLength()))
							{
								gb->SetFinished(true);
							}
						}
					}
				}

				// Send pause notification on demand
				if (reportPause && transfer.WritePrintPaused(pauseFilePosition, pauseReason))
				{
					reportPause = false;
				}
			}

			// Start the next transfer and wait for it to complete
			transfer.StartNextTransfer();
		}
		else if (!writingIap)
		{
			if (isConnected && (!transfer.IsConnected() || hadReset))
			{
				isConnected = false;
				numDisconnects++;
				if (!hadReset)
				{
					numTimeouts++;
				}
				reprap.GetPlatform().Message(NetworkInfoMessage, "Lost connection to Linux\n");

				rxPointer = txPointer = txEnd = 0;
				sendBufferUpdate = true;
				iapWritePointer = IAP_IMAGE_START;

				if (!requestedFileName.IsEmpty())
				{
					requestedFileDataLength = -1;
					requestedFileSemaphore.Give();
				}

				if (fileOperation != FileOperation::none)
				{
					fileOperation = FileOperation::none;
					fileSemaphore.Give();
				}
				MassStorage::InvalidateAllFiles();

				// Don't cache any messages if they cannot be sent
				{
					MutexLocker lock(gcodeReplyMutex);
					gcodeReply.ReleaseAll();
				}

				// Close all open G-code files
				for (size_t i = 0; i < NumGCodeChannels; i++)
				{
					GCodeBuffer *gb = reprap.GetGCodes().GetGCodeBuffer(GCodeChannel(i));
					if (gb->IsWaitingForMacro())
					{
						gb->ResolveMacroRequest(true, false);
					}

					MutexLocker locker(gb->mutex);
					if (gb->IsMacroRequestPending())
					{
						gb->MacroRequestSent();
					}
					gb->AbortFile(true, false);
					gb->MessageAcknowledged(true);
				}

				// Abort the print (if applicable)
				printAborted = true;

				// Turn off all the heaters
				reprap.GetHeat().SwitchOffAll(true);

				// Reset the SPI connection
				transfer.ResetConnection();

				if (hadReset)
				{
					// Let the main task invalidate resources
					TaskBase::Take(LinuxYieldTimeout);
				}
			}
			else if (isConnected || transfer.IsConnected())
			{
				// Wait for the next SPI transaction to complete or for a timeout to occur
				TaskBase::Take(SpiConnectionTimeout);
			}
			else
			{
				// Wait indefinitely for the next transfer
				TaskBase::Take();
			}
		}
	}
}

void LinuxInterface::Diagnostics(MessageType mtype) noexcept
{
	reprap.GetPlatform().Message(mtype, "=== SBC interface ===\n");
	transfer.Diagnostics(mtype);
	reprap.GetPlatform().MessageF(mtype, "Disconnects: %" PRIu32 ", timeouts: %" PRIu32 ", IAP RAM available 0x%05" PRIx32 "\n", numDisconnects, numTimeouts, iapRamAvailable);
	reprap.GetPlatform().MessageF(mtype, "Buffer RX/TX: %d/%d-%d\n", (int)rxPointer, (int)txPointer, (int)txEnd);
#ifdef TRACK_FILE_CODES
	reprap.GetPlatform().MessageF(mtype, "File codes read/handled: %d/%d, file macros open/closing: %d %d\n", (int)fileCodesRead, (int)fileCodesHandled, (int)fileMacrosRunning, (int)fileMacrosClosing);
#endif
}

GCodeResult LinuxInterface::HandleM576(GCodeBuffer& gb, const StringRef& reply) noexcept
{
	bool seen = false;

	if (gb.Seen('S'))
	{
		uint32_t sParam = gb.GetUIValue();
		if (sParam > SpiConnectionTimeout)
		{
			reply.printf("SPI transfer delay must not exceed %" PRIu32 "ms", SpiConnectionTimeout);
			return GCodeResult::error;
		}
		maxDelayBetweenTransfers = sParam;
		seen = true;
	}

	if (gb.Seen('F'))
	{
		uint32_t fParam = gb.GetUIValue();
		if (fParam > SpiConnectionTimeout)
		{
			reply.printf("SPI transfer delay must not exceed %" PRIu32 "ms", SpiConnectionTimeout);
			return GCodeResult::error;
		}
		maxFileOpenDelay = fParam;
		seen = true;
	}

	if (gb.Seen('P'))
	{
		numMaxEvents = gb.GetUIValue();
		seen = true;
	}

	if (!seen)
	{
		reply.printf("Max transfer delay %" PRIu32 "ms, max number of events during delays: %" PRIu32, maxDelayBetweenTransfers, numMaxEvents);
	}
	return GCodeResult::ok;
}

bool LinuxInterface::FillBuffer(GCodeBuffer &gb) noexcept
{
	if (gb.IsInvalidated() || gb.IsMacroFileClosed() || gb.IsMessageAcknowledged() ||
		gb.IsAbortRequested() || (reportPause && gb.GetChannel() == GCodeChannel::File) ||
		(gb.LatestMachineState().waitingForAcknowledgement && gb.IsMessagePromptPending()))
	{
		// Don't process codes that are supposed to be suspended...
		return false;
	}

	bool gotCommand = false;
	{
		//TODO can we take the lock inside the loop body instead, if we re-read readPointer and writePointer after taking it?
		TaskCriticalSectionLocker locker;
		if (rxPointer != txPointer || txEnd != 0)
		{
			bool updateRxPointer = true;
			uint16_t readPointer = rxPointer;
			do
			{
				BufferedCodeHeader *bufHeader = reinterpret_cast<BufferedCodeHeader*>(codeBuffer + readPointer);
				readPointer += sizeof(BufferedCodeHeader);
				const CodeHeader *codeHeader = reinterpret_cast<const CodeHeader*>(codeBuffer + readPointer);
				readPointer += bufHeader->length;

				RRF_ASSERT(bufHeader->length > 0);
				RRF_ASSERT(readPointer <= SpiCodeBufferSize);

				if (bufHeader->isPending)
				{
					if (gb.GetChannel().RawValue() == codeHeader->channel)
					{
#ifdef TRACK_FILE_CODES
						if (gb.GetChannel() == GCodeChannel::File && gb.GetCommandLetter() != 'Q')
						{
							fileMacrosRunning -= fileMacrosClosing;
							fileMacrosClosing = 0;
							if (fileCodesRead > fileCodesHandled + fileMacrosRunning)
							{
								// Note that we cannot use MessageF here because the task scheduler is suspended
								OutputBuffer *buf;
								if (OutputBuffer::Allocate(buf))
								{
									String<SHORT_GCODE_LENGTH> codeString;
									gb.PrintCommand(codeString.GetRef());
									buf->printf("Code %s did not return a code result, delta %d, running macros %d\n", codeString.c_str(), fileCodesRead - fileCodesHandled - fileMacrosRunning, fileMacrosRunning);
									gcodeReply.Push(buf, WarningMessage);
								}
								fileCodesRead = fileCodesHandled - fileMacrosRunning;
							}
							fileCodesRead++;
						}
#endif

						// Process the next binary G-code
						gb.PutBinary(reinterpret_cast<const uint32_t *>(codeHeader), bufHeader->length / sizeof(uint32_t));
						bufHeader->isPending = false;

						// Check if we can reset the ring buffer pointers
						if (updateRxPointer)
						{
							sendBufferUpdate = true;
							if (readPointer == txPointer && txEnd == 0)
							{
								// Buffer completely read, reset RX/TX pointers
								rxPointer = txPointer = 0;
							}
							else if (readPointer == txEnd)
							{
								// Read last code before overlapping, restart from the beginning
								rxPointer = txEnd = 0;
							}
							else
							{
								// Code has been read, move on to the next one
								rxPointer = readPointer;
							}
						}

						gotCommand = true;
						break;
					}
					updateRxPointer = false;
				}

				if (readPointer == txEnd)
				{
					if (updateRxPointer)
					{
						// Skipped non-pending codes, restart from the beginning
						rxPointer = txEnd = 0;
					}

					// About to overlap, continue from the start
					readPointer = 0;
				}
			} while (readPointer != txPointer);
		}
	}

	if (gotCommand)
	{
		gb.DecodeCommand();
		return true;
	}
	return false;
}

bool LinuxInterface::FileExists(const char *filename) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	filePath = filename;
	fileOperation = FileOperation::checkFileExists;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Return the result
	return fileSuccess;
}

bool LinuxInterface::DeleteFileOrDirectory(const char *fileOrDirectory) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	filePath = fileOrDirectory;
	fileOperation = FileOperation::deleteFileOrDirectory;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Return the result
	return fileSuccess;
}

FileHandle LinuxInterface::OpenFile(const char *filename, OpenMode mode, FilePosition& fileLength, uint32_t preAllocSize) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	filePath = filename;
	filePreAllocSize = preAllocSize;
	switch (mode)
	{
	case OpenMode::read:
		fileOperation = FileOperation::openRead;
		break;

	case OpenMode::write:
	case OpenMode::writeWithCrc:
		fileOperation = FileOperation::openWrite;
		break;

	case OpenMode::append:
		fileOperation = FileOperation::openAppend;
		break;

	default:
		filePath = nullptr;
		REPORT_INTERNAL_ERROR;
		break;
	}
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Update the file length and return the handle
	fileLength = fileOffset;
	return fileHandle;
}

int LinuxInterface::ReadFile(FileHandle handle, char *buffer, size_t bufferLength) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	fileHandle = handle;
	fileReadBuffer = buffer;
	fileBufferLength = bufferLength;
	fileOperation = FileOperation::read;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();
;

	// Return the number of bytes read
	return fileSuccess ? (int)fileOffset : -1;
}

bool LinuxInterface::WriteFile(FileHandle handle, const char *buffer, size_t bufferLength) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	fileHandle = handle;
	fileWriteBuffer = buffer;
	fileBufferLength = bufferLength;
	fileOperation = FileOperation::write;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Return the result
	return fileSuccess;
}

bool LinuxInterface::SeekFile(FileHandle handle, FilePosition offset) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	fileHandle = handle;
	fileOffset = offset;
	fileOperation = FileOperation::seek;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Return the result
	return fileSuccess;
}

bool LinuxInterface::TruncateFile(FileHandle handle) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return false;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	fileHandle = handle;
	fileOperation = FileOperation::truncate;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();

	// Return the result
	return fileSuccess;
}

void LinuxInterface::CloseFile(FileHandle handle) noexcept
{
	// Don't do anything if the SBC is not connected
	if (!IsConnected())
	{
		return;
	}

	// Set up the request content
	MutexLocker locker(fileMutex);
	fileHandle = handle;
	fileOperation = FileOperation::close;
	fileOperationPending = true;

	// Let the SBC task process this request as quickly as possible
	if (delaying)
	{
		delaying = false;
		sbcTask->Give();
	}
	fileSemaphore.Take();
}

void LinuxInterface::HandleGCodeReply(MessageType mt, const char *reply) noexcept
{
	if (!IsConnected())
	{
		return;
	}

#ifdef TRACK_FILE_CODES
	if ((mt & (1 << GCodeChannel::File)) != 0)
	{
		fileCodesHandled++;
	}
#endif

	MutexLocker lock(gcodeReplyMutex);
	OutputBuffer *buffer = gcodeReply.GetLastItem();
	if (buffer != nullptr && mt == gcodeReply.GetLastItemType() && (mt & PushFlag) != 0 && !buffer->IsReferenced())
	{
		// Try to save some space by combining segments that have the Push flag set
		buffer->cat(reply);
	}
	else if (reply[0] != 0 && OutputBuffer::Allocate(buffer))
	{
		// Attempt to allocate one G-code buffer per non-empty output message
		buffer->cat(reply);
		gcodeReply.Push(buffer, mt);
	}
	else
	{
		// Store nullptr to indicate an empty response. This way many OutputBuffer references can be saved
		gcodeReply.Push(nullptr, mt);
	}
	EventOccurred();
}

void LinuxInterface::HandleGCodeReply(MessageType mt, OutputBuffer *buffer) noexcept
{
	if (!IsConnected())
	{
		OutputBuffer::ReleaseAll(buffer);
		return;
	}

#ifdef TRACK_FILE_CODES
	if ((mt & (1 << GCodeChannel::File)) != 0)
	{
		fileCodesHandled++;
	}
#endif

	MutexLocker lock(gcodeReplyMutex);
	gcodeReply.Push(buffer, mt);
	EventOccurred();
}

// Read a file chunk from the SBC. When a response has been received, the current task is woken up again.
// It changes bufferLength to the number of received bytes
// This method returns true on success and false if an error occurred (e.g. file not found)
bool LinuxInterface::GetFileChunk(const char *filename, uint32_t offset, char *buffer, uint32_t& bufferLength, uint32_t& fileLength) noexcept
{
	if (waitingForFileChunk)
	{
		reprap.GetPlatform().Message(ErrorMessage, "Trying to request a file chunk from two independent tasks\n");
		bufferLength = fileLength = 0;
		return false;
	}

	fileChunkRequestSent = false;
	requestedFileName.copy(filename);
	requestedFileLength = bufferLength;
	requestedFileOffset = offset;
	requestedFileBuffer = buffer;

	waitingForFileChunk = true;
	requestedFileSemaphore.Take();

	waitingForFileChunk = false;
	if (requestedFileDataLength < 0)
	{
		bufferLength = fileLength = 0;
		return false;
	}
	bufferLength = requestedFileDataLength;
	fileLength = requestedFileLength;
	return true;
}

void LinuxInterface::EventOccurred(bool timeCritical) noexcept
{
	if (!IsConnected())
	{
		return;
	}

	// Increment the number of events
	if (timeCritical)
	{
		numEvents = numMaxEvents;
	}
	else
	{
		numEvents++;
	}

	// Stop delaying if the next transfer is time-critical
	if (delaying && numEvents >= numMaxEvents)
	{
		delaying = false;
		sbcTask->Give();
	}
}

void LinuxInterface::InvalidateBufferedCodes(GCodeChannel channel) noexcept
{
	TaskCriticalSectionLocker locker;
	if (rxPointer != txPointer || txEnd != 0)
	{
		bool updateRxPointer = true;
		uint16_t readPointer = rxPointer;
		do
		{
			BufferedCodeHeader *bufHeader = reinterpret_cast<BufferedCodeHeader *>(codeBuffer + readPointer);
			if (bufHeader->isPending)
			{
				const CodeHeader *codeHeader = reinterpret_cast<const CodeHeader*>(codeBuffer + readPointer + sizeof(BufferedCodeHeader));
				if (codeHeader->channel == channel.RawValue())
				{
					bufHeader->isPending = false;
				}
				else
				{
					updateRxPointer = false;
				}
			}
			readPointer += sizeof(BufferedCodeHeader) + bufHeader->length;

			if (updateRxPointer)
			{
				sendBufferUpdate = true;
				if (readPointer == txPointer && txEnd == 0)
				{
					// Buffer is empty again, reset the pointers
					rxPointer = txPointer = 0;
					break;
				}
				else if (readPointer == txEnd)
				{
					// Invalidated last code before overlapping, continue from the beginning
					readPointer = 0;
					rxPointer = txEnd = 0;
				}
				else
				{
					// Invalidated next code
					rxPointer = readPointer;
				}
			}
			else if (readPointer == txEnd)
			{
				// About to overlap, continue from the start
				readPointer = 0;
			}
		} while (readPointer != txPointer);
	}
}

#endif
