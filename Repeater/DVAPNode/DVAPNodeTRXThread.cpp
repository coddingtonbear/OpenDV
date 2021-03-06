/*
 *   Copyright (C) 2011-2014 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "DVAPNodeTRXThread.h"
#include "DStarDefines.h"
#include "DVAPNodeApp.h"
#include "HeaderData.h"
#include "Version.h"

const unsigned char DTMF_MASK[] = {0x82U, 0x08U, 0x20U, 0x82U, 0x00U, 0x00U, 0x82U, 0x00U, 0x00U};
const unsigned char DTMF_SIG[]  = {0x82U, 0x08U, 0x20U, 0x82U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

const unsigned int NETWORK_QUEUE_COUNT = 2U;

const unsigned int SILENCE_THRESHOLD = 2U;

const unsigned int CYCLE_TIME = 9U;

CDVAPNodeTRXThread::CDVAPNodeTRXThread() :
m_dvap(NULL),
m_protocolHandler(NULL),
m_stopped(true),
m_rptCallsign(),
m_gwyCallsign(),
m_beacon(NULL),
m_announcement(NULL),
m_recordRPT1(),
m_recordRPT2(),
m_deleteRPT1(),
m_deleteRPT2(),
m_rxHeader(NULL),
m_localQueue((DV_FRAME_LENGTH_BYTES + 2U) * 50U, LOCAL_RUN_FRAME_COUNT),			// 1s worth of data
m_networkQueue(NULL),
m_writeNum(0U),
m_readNum(0U),
m_radioSeqNo(0U),
m_networkSeqNo(0U),
m_timeoutTimer(1000U, 180U),		// 180s
m_watchdogTimer(1000U, 2U),			// 2s
m_pollTimer(1000U, 60U),			// 60s
m_ackTimer(1000U, 0U, 500U),		// 0.5s
m_beaconTimer(1000U, 600U),			// 10 mins
m_announcementTimer(1000U, 0U),		// not running
m_dvapPollTimer(1000U, 2U),			// 2s
m_state(DSRS_LISTENING),
m_ackEncoder(),
m_linkEncoder(),
m_tx(false),
m_squelch(false),
m_signal(0),
m_killed(false),
m_mode(MODE_SIMPLEX),
m_ack(AT_BER),
m_restriction(false),
m_rpt1Validation(true),
m_logging(NULL),
m_lastData(NULL),
m_ambe(),
m_ambeFrames(0U),
m_ambeSilence(0U),
m_ambeBits(1U),
m_ambeErrors(0U),
m_lastAMBEBits(0U),
m_lastAMBEErrors(0U),
m_ackText(),
m_tempAckText(),
m_linkStatus(LS_NONE),
m_reflector(),
m_regEx(wxT("^[A-Z0-9]{1}[A-Z0-9]{0,1}[0-9]{1,2}[A-Z]{1,4} {0,4}[ A-Z]{1}$")),
m_headerTime(),
m_packetTime(),
m_packetCount(0U),
m_packetSilence(0U),
m_blackList(NULL),
m_greyList(NULL),
m_blocked(false),
m_recording(false),
m_deleting(false)
{
	m_networkQueue = new COutputQueue*[NETWORK_QUEUE_COUNT];
	for (unsigned int i = 0U; i < NETWORK_QUEUE_COUNT; i++)
		m_networkQueue[i] = new COutputQueue((DV_FRAME_LENGTH_BYTES + 2U) * 200U, NETWORK_RUN_FRAME_COUNT);		// 4s worth of data);

	m_lastData = new unsigned char[DV_FRAME_MAX_LENGTH_BYTES];

	setRepeaterState(DSRS_LISTENING);
}

CDVAPNodeTRXThread::~CDVAPNodeTRXThread()
{
	for (unsigned int i = 0U; i < NETWORK_QUEUE_COUNT; i++)
		delete m_networkQueue[i];
	delete[] m_networkQueue;
	delete[] m_lastData;
	delete   m_rxHeader;
}

void CDVAPNodeTRXThread::run()
{
	// Wait here until we have the essentials to run
	while (!m_killed && (m_dvap == NULL || m_rptCallsign.IsEmpty() || m_rptCallsign.IsSameAs(wxT("        "))))
		::wxMilliSleep(500UL);		// 1/2 sec

	if (m_killed)
		return;

	m_stopped = false;

	m_beaconTimer.start();
	m_dvapPollTimer.start();
	m_announcementTimer.start();

	if (m_protocolHandler != NULL)
		m_pollTimer.start();

	wxLogMessage(wxT("Starting the DVAP node thread"));

	wxStopWatch stopWatch;

	while (!m_killed) {
		stopWatch.Start();

		switch (m_state) {
			case DSRS_LISTENING:
				receiveRadioHeader();
				break;
			case DSRS_NETWORK:
				break;
			case DSRS_VALID:
			case DSRS_INVALID:
			case DSRS_TIMEOUT:
				receiveRadioData();
				break;
			default:		// All the DSRS_*_WAIT values
				break;
		}

		// Listen all the time on the network for status packets at least
		receiveNetwork();

		repeaterStateMachine();

		m_tx      = m_dvap->getPTT();
		m_squelch = m_dvap->getSquelch();
		m_signal  = m_dvap->getSignal();

		// Send the network poll if needed and restart the timer
		if (m_pollTimer.hasExpired()) {
#if defined(__WINDOWS__)
			m_protocolHandler->writePoll(wxT("win_dvap-") + VERSION);
#else
			m_protocolHandler->writePoll(wxT("linux_dvap-") + VERSION);
#endif
			m_pollTimer.reset();
		}

		if (m_dvapPollTimer.hasExpired()) {
			m_dvap->writePoll();
			m_dvapPollTimer.reset();
		}

		// Send the beacon and restart the timer
		if (m_beaconTimer.isRunning() && m_beaconTimer.hasExpired()) {
			m_beacon->sendBeacon();
			m_beaconTimer.reset();
		}

		// Send the announcement and restart the timer
		if (m_announcementTimer.isRunning() && m_announcementTimer.hasExpired()) {
			m_announcement->startAnnouncement();
			m_announcementTimer.reset();
		}

		if (m_localQueue.dataReady())
			transmitLocalData();
		else if (m_networkQueue[m_readNum]->dataReady())
			transmitNetworkData();
		else if (m_localQueue.headerReady())
			transmitLocalHeader();
		else if (m_networkQueue[m_readNum]->headerReady())
			transmitNetworkHeader();

		unsigned long ms = stopWatch.Time();
		if (ms < CYCLE_TIME) {
			::wxMilliSleep(CYCLE_TIME - ms);
			clock(CYCLE_TIME);
		} else {
			clock(ms);
		}
	}

	wxLogMessage(wxT("Stopping the DVAP node thread"));

	m_dvap->close();

	if (m_logging != NULL) {
		m_logging->close();
		delete m_logging;
	}

	delete m_beacon;
	delete m_announcement;

	delete m_blackList;
	delete m_greyList;

	if (m_protocolHandler != NULL) {
		m_protocolHandler->close();
		delete m_protocolHandler;
	}
}

void CDVAPNodeTRXThread::kill()
{
	m_killed = true;
}

void CDVAPNodeTRXThread::setCallsign(const wxString& callsign, const wxString& gateway, DSTAR_MODE mode, ACK_TYPE ack, bool restriction, bool rpt1Validation)
{
	// Pad the callsign up to eight characters
	m_rptCallsign = callsign;
	m_rptCallsign.resize(LONG_CALLSIGN_LENGTH, wxT(' '));

	if (gateway.IsEmpty()) {
		m_gwyCallsign = callsign;
		m_gwyCallsign.resize(LONG_CALLSIGN_LENGTH - 1U, wxT(' '));
		m_gwyCallsign.Append(wxT("G"));
	} else {
		m_gwyCallsign = gateway;
		m_gwyCallsign.resize(LONG_CALLSIGN_LENGTH, wxT(' '));
	}

	m_mode           = mode;
	m_ack            = ack;
	m_restriction    = restriction;
	m_rpt1Validation = rpt1Validation;
}

void CDVAPNodeTRXThread::setProtocolHandler(CRepeaterProtocolHandler* handler)
{
	wxASSERT(handler != NULL);

	m_protocolHandler = handler;
}

void CDVAPNodeTRXThread::setDVAP(CDVAPController* dvap)
{
	wxASSERT(dvap != NULL);

	m_dvap = dvap;
}

void CDVAPNodeTRXThread::setTimes(unsigned int timeout, unsigned int ackTime)
{
	m_timeoutTimer.setTimeout(timeout);
	m_ackTimer.setTimeout(0U, ackTime);
}

void CDVAPNodeTRXThread::setBeacon(unsigned int time, const wxString& text, bool voice, TEXT_LANG language)
{
	m_beaconTimer.setTimeout(time);

	if (time > 0U)
		m_beacon = new CBeaconUnit(this, m_rptCallsign, text, voice, language);
}

void CDVAPNodeTRXThread::setAnnouncement(bool enabled, unsigned int time, const wxString& recordRPT1, const wxString& recordRPT2, const wxString& deleteRPT1, const wxString& deleteRPT2)
{
	if (enabled && time > 0U) {
		m_announcement = new CAnnouncementUnit(this, m_rptCallsign);

		m_announcementTimer.setTimeout(time);

		m_recordRPT1 = recordRPT1;
		m_recordRPT2 = recordRPT2;
		m_deleteRPT1 = deleteRPT1;
		m_deleteRPT2 = deleteRPT2;

		m_recordRPT1.Append(wxT(' '), LONG_CALLSIGN_LENGTH);
		m_recordRPT2.Append(wxT(' '), LONG_CALLSIGN_LENGTH);
		m_deleteRPT1.Append(wxT(' '), LONG_CALLSIGN_LENGTH);
		m_deleteRPT2.Append(wxT(' '), LONG_CALLSIGN_LENGTH);

		m_recordRPT1.Truncate(LONG_CALLSIGN_LENGTH);
		m_recordRPT2.Truncate(LONG_CALLSIGN_LENGTH);
		m_deleteRPT1.Truncate(LONG_CALLSIGN_LENGTH);
		m_deleteRPT2.Truncate(LONG_CALLSIGN_LENGTH);
	}
}

void CDVAPNodeTRXThread::setLogging(bool logging, const wxString& dir)
{
	if (logging && m_logging == NULL) {
		m_logging = new CDVTOOLFileWriter;
		m_logging->setDirectory(dir);
		return;
	}

	if (!logging && m_logging != NULL) {
		delete m_logging;
		m_logging = NULL;
		return;
	}
}

void CDVAPNodeTRXThread::setBlackList(CCallsignList* list)
{
	wxASSERT(list != NULL);

	m_blackList = list;
}

void CDVAPNodeTRXThread::setGreyList(CCallsignList* list)
{
	wxASSERT(list != NULL);

	m_greyList = list;
}

void CDVAPNodeTRXThread::receiveRadioHeader()
{
	CHeaderData* header = m_dvap->readHeader();
	if (header == NULL)
		return;

	wxLogMessage(wxT("Header decoded - My: %s/%s  Your: %s  Rpt1: %s  Rpt2: %s  Flags: %02X %02X %02X"), header->getMyCall1().c_str(), header->getMyCall2().c_str(), header->getYourCall().c_str(), header->getRptCall1().c_str(), header->getRptCall2().c_str(), header->getFlag1(), header->getFlag2(), header->getFlag3());

	bool res = processRadioHeader(header);
	if (res) {
		// A valid header and is a DV packet
		m_radioSeqNo     = 20U;
		m_ambeFrames     = 0U;
		m_ambeSilence    = 0U;
		m_ambeBits       = 1U;
		m_ambeErrors     = 0U;
		m_lastAMBEBits   = 0U;
		m_lastAMBEErrors = 0U;
	} else {
		// This is a DD packet or some other problem
		// wxLogMessage(wxT("Invalid header"));
	}
}

void CDVAPNodeTRXThread::receiveRadioData()
{
	unsigned char data[DV_FRAME_MAX_LENGTH_BYTES];
	bool end;

	int length = m_dvap->readData(data, DV_FRAME_MAX_LENGTH_BYTES, end);
	if (length == -1)
		return;

	if (end) {
		// wxLogMessage(wxT("Found end sync"));
		processRadioFrame(data, FRAME_END);
		endOfRadioData();
		return;
	}

	bool res = ::memcmp(data + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES) == 0;
	if (res) {
		// wxLogMessage(wxT("Found data sync at frame %u"), m_radioSyncCounter);
		m_radioSeqNo = 0U;
		processRadioFrame(data, FRAME_SYNC);
	} else if (m_radioSeqNo == 20U) {
		// wxLogMessage(wxT("Regenerating data sync"));
		m_radioSeqNo = 0U;
		processRadioFrame(data, FRAME_SYNC);
	} else {
		m_radioSeqNo++;
		processRadioFrame(data, FRAME_NORMAL);
	}
}

void CDVAPNodeTRXThread::receiveNetwork()
{
	if (m_protocolHandler == NULL)
		return;

	NETWORK_TYPE type;

	for (;;) {
		type = m_protocolHandler->read();

		// Get the data from the network
		if (type == NETWORK_NONE) {					// Nothing received
			break;
		} else if (type == NETWORK_HEADER) {		// A header
			CHeaderData* header = m_protocolHandler->readHeader();
			if (header != NULL) {
				::memcpy(m_lastData, NULL_FRAME_DATA_BYTES, DV_FRAME_LENGTH_BYTES);

				processNetworkHeader(header);

				m_headerTime.Start();
				m_packetTime.Start();
				m_packetCount   = 0U;
				m_packetSilence = 0U;
			}
		} else if (type == NETWORK_DATA) {			// AMBE data and slow data
			unsigned char data[2U * DV_FRAME_MAX_LENGTH_BYTES];
			::memset(data, 0x00U, 2U * DV_FRAME_MAX_LENGTH_BYTES);

			unsigned char seqNo;
			unsigned int length = m_protocolHandler->readData(data, DV_FRAME_MAX_LENGTH_BYTES, seqNo);
			if (length != 0U) {
				::memcpy(m_lastData, data, length);
				m_packetCount += processNetworkFrame(data, length, seqNo);
				m_watchdogTimer.reset();
			}
		} else if (type == NETWORK_TEXT) {			// Slow data text for the Ack
			m_protocolHandler->readText(m_ackText, m_linkStatus, m_reflector);
			m_linkEncoder.setTextData(m_ackText);
			wxLogMessage(wxT("Slow data set to \"%s\""), m_ackText.c_str());
		} else if (type == NETWORK_TEMPTEXT) {			// Temporary slow data text for the Ack
			m_protocolHandler->readTempText(m_tempAckText);
			wxLogMessage(wxT("Temporary slow data set to \"%s\""), m_tempAckText.c_str());
		} else if (type == NETWORK_STATUS1) {			// Slow data text for status 1, unused
			m_protocolHandler->readStatus1();
		} else if (type == NETWORK_STATUS2) {			// Slow data text for status 2, unused
			m_protocolHandler->readStatus2();
		} else if (type == NETWORK_STATUS3) {			// Slow data text for status 3, unused
			m_protocolHandler->readStatus3();
		} else if (type == NETWORK_STATUS4) {			// Slow data text for status 4, unused
			m_protocolHandler->readStatus4();
		} else if (type == NETWORK_STATUS5) {			// Slow data text for status 5, unused
			m_protocolHandler->readStatus5();
		}
	}

	// Have we missed any data frames?
	if (m_state == DSRS_NETWORK && m_packetTime.Time() > 200L) {
		unsigned int packetsNeeded = m_headerTime.Time() / DSTAR_FRAME_TIME_MS;

		// wxLogMessage(wxT("Time: %u ms, need %u packets and received %u packets"), ms - m_headerMS, packetsNeeded, m_packetCount);

		if (packetsNeeded > m_packetCount) {
			unsigned int count = packetsNeeded - m_packetCount;

			if (count > 5U) {
				count -= 2U;

				// wxLogMessage(wxT("Inserting %u silence packets into the network data stream"), count);

				// Create silence frames
				for (unsigned int i = 0U; i < count; i++) {
					unsigned char data[DV_FRAME_LENGTH_BYTES];
					::memcpy(data, NULL_FRAME_DATA_BYTES, DV_FRAME_LENGTH_BYTES);
					m_packetCount += processNetworkFrame(data, DV_FRAME_LENGTH_BYTES, m_networkSeqNo);
					m_packetSilence++;
				}
			}
		}

		m_packetTime.Start();
	}
}

void CDVAPNodeTRXThread::transmitLocalHeader(CHeaderData* header)
{
	wxLogMessage(wxT("Transmitting to - My: %s/%s  Your: %s  Rpt1: %s  Rpt2: %s  Flags: %02X %02X %02X"), header->getMyCall1().c_str(), header->getMyCall2().c_str(), header->getYourCall().c_str(), header->getRptCall1().c_str(), header->getRptCall2().c_str(), header->getFlag1(), header->getFlag2(), header->getFlag3());

	m_localQueue.reset();
	m_localQueue.setHeader(header);
}

void CDVAPNodeTRXThread::transmitBeaconHeader()
{
	CHeaderData* header = new CHeaderData(m_rptCallsign, wxT("RPTR"), wxT("CQCQCQ  "), m_gwyCallsign, m_rptCallsign);
	transmitLocalHeader(header);
}

void CDVAPNodeTRXThread::transmitBeaconData(const unsigned char* data, unsigned int length, bool end)
{
	m_localQueue.addData(data, length, end);
}

void CDVAPNodeTRXThread::transmitAnnouncementHeader(CHeaderData* header)
{
	header->setRptCall1(m_gwyCallsign);
	header->setRptCall2(m_rptCallsign);

	transmitLocalHeader(header);
}

void CDVAPNodeTRXThread::transmitAnnouncementData(const unsigned char* data, unsigned int length, bool end)
{
	m_localQueue.addData(data, length, end);
}

void CDVAPNodeTRXThread::transmitNetworkHeader(CHeaderData* header)
{
	wxLogMessage(wxT("Transmitting to - My: %s/%s  Your: %s  Rpt1: %s  Rpt2: %s  Flags: %02X %02X %02X"), header->getMyCall1().c_str(), header->getMyCall2().c_str(), header->getYourCall().c_str(), header->getRptCall1().c_str(), header->getRptCall2().c_str(), header->getFlag1(), header->getFlag2(), header->getFlag3());

	bool empty = m_networkQueue[m_readNum]->isEmpty();
	if (!empty) {
		bool headerReady = m_networkQueue[m_readNum]->headerReady();
		if (headerReady) {
			// Transmission has never started, so just purge the queue
			m_networkQueue[m_readNum]->reset();

			m_readNum++;
			if (m_readNum >= NETWORK_QUEUE_COUNT)
				m_readNum = 0U;
		} else {
			// Purge the currently transmitting buffer and append an end of stream
			m_dvap->purgeTX();
			m_networkQueue[m_readNum]->reset();
			m_networkQueue[m_readNum]->addData(END_PATTERN_BYTES, DV_FRAME_LENGTH_BYTES, true);
		}
	}

	m_networkQueue[m_writeNum]->reset();
	m_networkQueue[m_writeNum]->setHeader(header);
}

void CDVAPNodeTRXThread::transmitStatus()
{
	CHeaderData* header = new CHeaderData(m_rptCallsign, wxT("    "), m_rxHeader->getMyCall1(), m_gwyCallsign, m_rptCallsign, RELAY_UNAVAILABLE);
	transmitLocalHeader(header);

	// Filler data
	for (unsigned int i = 0U; i < 21U; i++) {
		unsigned char buffer[DV_FRAME_LENGTH_BYTES];

		if (i == 0U) {
			m_ackEncoder.sync();

			::memcpy(buffer + 0U, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
			::memcpy(buffer + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES);
		} else {
			unsigned char text[DATA_FRAME_LENGTH_BYTES];
			m_ackEncoder.getTextData(text);

			::memcpy(buffer + 0U, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
			::memcpy(buffer + VOICE_FRAME_LENGTH_BYTES, text, DATA_FRAME_LENGTH_BYTES);
		}

		m_localQueue.addData(buffer, DV_FRAME_LENGTH_BYTES, false);
	}

	m_localQueue.addData(END_PATTERN_BYTES, DV_FRAME_LENGTH_BYTES, true);
}

void CDVAPNodeTRXThread::transmitErrorStatus()
{
	CHeaderData* header = new CHeaderData(m_rptCallsign, wxT("    "), m_rxHeader->getMyCall1(), m_rptCallsign, m_rptCallsign, RELAY_UNAVAILABLE);
	transmitLocalHeader(header);

	// Filler data
	for (unsigned int i = 0U; i < 21U; i++) {
		unsigned char buffer[DV_FRAME_LENGTH_BYTES];

		if (i == 0U) {
			m_linkEncoder.sync();

			::memcpy(buffer + 0U, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
			::memcpy(buffer + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES);
		} else {
			unsigned char text[DATA_FRAME_LENGTH_BYTES];
			m_linkEncoder.getTextData(text);

			::memcpy(buffer + 0U, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
			::memcpy(buffer + VOICE_FRAME_LENGTH_BYTES, text, DATA_FRAME_LENGTH_BYTES);
		}

		m_localQueue.addData(buffer, DV_FRAME_LENGTH_BYTES, false);
	}

	m_localQueue.addData(END_PATTERN_BYTES, DV_FRAME_LENGTH_BYTES, true);
}

void CDVAPNodeTRXThread::transmitLocalHeader()
{
	// Don't send a header until the TX has stopped
	if (m_tx)
		return;

	CHeaderData* header = m_localQueue.getHeader();
	if (header == NULL)
		return;

	m_dvap->writeHeader(*header);
	delete header;

	m_dvapPollTimer.reset();
}

void CDVAPNodeTRXThread::transmitLocalData()
{
	bool space = m_dvap->hasSpace();
	if (!space)
		return;

	unsigned char buffer[DV_FRAME_LENGTH_BYTES];
	bool end;

	unsigned int length = m_localQueue.getData(buffer, DV_FRAME_LENGTH_BYTES, end);

	if (length > 0U) {
		m_dvap->writeData(buffer, length, end);
		m_dvapPollTimer.reset();

		if (end)
			m_localQueue.reset();
	}
}

void CDVAPNodeTRXThread::transmitNetworkHeader()
{
	// Don't send a header until the TX has stopped
	if (m_tx)
		return;

	CHeaderData* header = m_networkQueue[m_readNum]->getHeader();
	if (header == NULL)
		return;

	m_dvap->writeHeader(*header);
	delete header;

	m_dvapPollTimer.reset();
}

void CDVAPNodeTRXThread::transmitNetworkData()
{
	bool space = m_dvap->hasSpace();
	if (!space)
		return;

	unsigned char buffer[DV_FRAME_LENGTH_BYTES];
	bool end;

	unsigned int length = m_networkQueue[m_readNum]->getData(buffer, DV_FRAME_LENGTH_BYTES, end);

	if (length > 0U) {
		m_dvap->writeData(buffer, length, end);
		m_dvapPollTimer.reset();

		if (end) {
			m_networkQueue[m_readNum]->reset();

			m_readNum++;
			if (m_readNum >= NETWORK_QUEUE_COUNT)
				m_readNum = 0U;
		}
	}
}

void CDVAPNodeTRXThread::repeaterStateMachine()
{
	switch (m_state) {
		case DSRS_VALID:
			if (m_timeoutTimer.isRunning() && m_timeoutTimer.hasExpired()) {
				wxLogMessage(wxT("User has timed out"));
				setRepeaterState(DSRS_TIMEOUT);
			}
			break;

		case DSRS_VALID_WAIT:
			if (m_ackTimer.hasExpired()) {
				transmitStatus();
				setRepeaterState(DSRS_LISTENING);
			}
			break;

		case DSRS_INVALID_WAIT:
			if (m_ackTimer.hasExpired()) {
				transmitErrorStatus();
				setRepeaterState(DSRS_LISTENING);
			}
			break;

		case DSRS_TIMEOUT_WAIT:
			if (m_ackTimer.hasExpired()) {
				transmitStatus();
				setRepeaterState(DSRS_LISTENING);
			}
			break;

		case DSRS_NETWORK:
			if (m_watchdogTimer.hasExpired()) {
				wxLogMessage(wxT("Network watchdog has expired"));
				// Send end of transmission data to the radio
				m_networkQueue[m_writeNum]->addData(END_PATTERN_BYTES, DV_FRAME_LENGTH_BYTES, true);
				endOfNetworkData();
			}
			break;

		default:
			break;
	}
}

bool CDVAPNodeTRXThread::setRepeaterState(DSTAR_RPT_STATE state)
{
	// The "from" state
	switch (m_state) {
		case DSRS_LISTENING:
			m_beaconTimer.stop();
			break;

		default:
			break;
	}

	// The "to" state
	switch (state) {
		case DSRS_LISTENING:
			m_timeoutTimer.stop();
			m_watchdogTimer.stop();
			m_ackTimer.stop();
			m_beaconTimer.start();
			m_state = DSRS_LISTENING;
			if (m_protocolHandler != NULL)	// Tell the protocol handler
				m_protocolHandler->reset();
			break;

		case DSRS_VALID:
			if (m_state != DSRS_LISTENING)
				return false;

			m_timeoutTimer.start();
			m_state = DSRS_VALID;
			break;

		case DSRS_VALID_WAIT:
			m_ackTimer.start();
			m_timeoutTimer.stop();
			m_state = DSRS_VALID_WAIT;
			break;

		case DSRS_TIMEOUT:
			// Send end of transmission data to the network
			if (!m_blocked && m_protocolHandler != NULL) {
				unsigned char bytes[DV_FRAME_MAX_LENGTH_BYTES];
				::memcpy(bytes, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
				::memcpy(bytes + VOICE_FRAME_LENGTH_BYTES, END_PATTERN_BYTES, END_PATTERN_LENGTH_BYTES);
				m_protocolHandler->writeData(bytes, DV_FRAME_MAX_LENGTH_BYTES, 0U, true);
			}

			m_timeoutTimer.stop();
			m_state = DSRS_TIMEOUT;
			break;

		case DSRS_TIMEOUT_WAIT:
			m_ackTimer.start();
			m_timeoutTimer.stop();
			m_state = DSRS_TIMEOUT_WAIT;
			break;

		case DSRS_INVALID:
			if (m_state != DSRS_LISTENING)
				return false;

			m_timeoutTimer.stop();
			m_state = DSRS_INVALID;
			break;

		case DSRS_INVALID_WAIT:
			m_ackTimer.start();
			m_timeoutTimer.stop();
			m_state = DSRS_INVALID_WAIT;
			break;

		case DSRS_NETWORK:
			if (m_state != DSRS_LISTENING && m_state != DSRS_VALID_WAIT && m_state != DSRS_INVALID_WAIT && m_state != DSRS_TIMEOUT_WAIT)
				return false;

			m_state = DSRS_NETWORK;
			m_networkSeqNo = 0U;
			m_timeoutTimer.stop();
			m_watchdogTimer.start();
			m_ackTimer.stop();
			break;

		default:
			break;
	}

	return true;
}

bool CDVAPNodeTRXThread::processRadioHeader(CHeaderData* header)
{
	wxASSERT(header != NULL);

	// Check announcement messages
	bool res = checkAnnouncements(*header);
	if (res) {
		bool res = setRepeaterState(DSRS_INVALID);
		if (res) {
			delete m_rxHeader;
			m_rxHeader = header;
		} else {
			delete header;
		}
		return true;
	}

	// We don't handle DD data packets
	if (header->isDataPacket()) {
		wxLogMessage(wxT("Received a DD packet, ignoring"));
		delete header;
		return false;
	}

	if (m_blackList != NULL) {
		bool res = m_blackList->isInList(header->getMyCall1());
		if (res) {
			wxLogMessage(wxT("%s rejected due to being in the black list"), header->getMyCall1().c_str());
			delete header;
			return true;
		}
	}

	m_blocked = false;
	if (m_greyList != NULL) {
		bool res = m_greyList->isInList(header->getMyCall1());
		if (res) {
			wxLogMessage(wxT("%s blocked from the network due to being in the grey list"), header->getMyCall1().c_str());
			m_blocked = true;
		}
	}

	TRISTATE valid = checkHeader(*header);
	switch (valid) {
		case STATE_FALSE: {
				bool res = setRepeaterState(DSRS_INVALID);
				if (res) {
					delete m_rxHeader;
					m_rxHeader = header;
				} else {
					delete header;
				}
			}
			return true;

		case STATE_UNKNOWN:
			delete header;
			return true;

		case STATE_TRUE:
			break;
	}

	res = setRepeaterState(DSRS_VALID);
	if (!res) {
		delete header;
		return true;
	}

	// Send the valid header to the gateway if we are accepted
	delete m_rxHeader;
	m_rxHeader = header;

	if (m_logging != NULL)
		m_logging->open(*m_rxHeader);

	// Only send on the network if they are not blocked and we have one and RPT2 is not blank or the repeater callsign
	if (!m_blocked && m_protocolHandler != NULL && !m_rxHeader->getRptCall2().IsSameAs(wxT("        ")) && !m_rxHeader->getRptCall2().IsSameAs(m_rptCallsign)) {
		CHeaderData netHeader(*m_rxHeader);
		netHeader.setRptCall1(m_rxHeader->getRptCall2());
		netHeader.setRptCall2(m_rxHeader->getRptCall1());
		netHeader.setFlag1(m_rxHeader->getFlag1() & ~REPEATER_MASK);
		m_protocolHandler->writeHeader(netHeader);
	}

	return true;
}

void CDVAPNodeTRXThread::processNetworkHeader(CHeaderData* header)
{
	wxASSERT(header != NULL);

	wxLogMessage(wxT("Network header received - My: %s/%s  Your: %s  Rpt1: %s  Rpt2: %s  Flags: %02X %02X %02X"), header->getMyCall1().c_str(), header->getMyCall2().c_str(), header->getYourCall().c_str(), header->getRptCall1().c_str(), header->getRptCall2().c_str(), header->getFlag1(), header->getFlag2(), header->getFlag3());

	// Is it for us?
	if (!header->getRptCall2().IsSameAs(m_rptCallsign)) {
		wxLogMessage(wxT("Invalid network RPT2 value, ignoring"));
		delete header;
		return;
	}

	bool res = setRepeaterState(DSRS_NETWORK);
	if (!res) {
		delete header;
		return;
	}

	delete m_rxHeader;
	m_rxHeader = header;

	CHeaderData* txHeader = new CHeaderData(*m_rxHeader);
	transmitNetworkHeader(txHeader);
}

void CDVAPNodeTRXThread::processRadioFrame(unsigned char* data, FRAME_TYPE type)
{
	m_ambeFrames++;

	// If a sync frame, regenerate the sync bytes
	if (type == FRAME_SYNC)
		::memcpy(data + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES);

	// Only regenerate the AMBE on received radio data
	unsigned int errors = 0U;
	if (type != FRAME_END) {
		errors = m_ambe.regenerate(data);
		m_ambeErrors += errors;
		m_ambeBits   += 48U;		// Only count the bits with FEC added
	}

	if (::memcmp(data, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES) == 0)
		m_ambeSilence++;

	// If this is deleting an announcement, ignore the audio
	if (m_deleting) {
		if (type == FRAME_END) {
			m_deleting  = false;
			m_recording = false;
		}
		return;
	}

	// If this is recording an announcement, send the audio to the announcement unit and then stop
	if (m_recording) {
		m_announcement->writeData(data, DV_FRAME_LENGTH_BYTES, type == FRAME_END);
		if (type == FRAME_END) {
			m_deleting  = false;
			m_recording = false;
		}
		return;
	}

	// Don't pass through the frame of an invalid transmission
	if (m_state != DSRS_VALID)
		return;

	if (type == FRAME_END) {
		if (m_logging != NULL)
			m_logging->close();

		// Send null data and the end marker over the network, and the statistics
		if (!m_blocked && m_protocolHandler != NULL) {
			unsigned char bytes[DV_FRAME_MAX_LENGTH_BYTES];
			::memcpy(bytes, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
			::memcpy(bytes + VOICE_FRAME_LENGTH_BYTES, END_PATTERN_BYTES, END_PATTERN_LENGTH_BYTES);
			m_protocolHandler->writeData(bytes, DV_FRAME_MAX_LENGTH_BYTES, 0U, true);
		}
	} else {
		if (m_logging != NULL)
			m_logging->write(data, DV_FRAME_LENGTH_BYTES);

		// Send the data to the network
		if (!m_blocked && m_protocolHandler != NULL)
			m_protocolHandler->writeData(data, DV_FRAME_LENGTH_BYTES, errors, false);
	}
}

unsigned int CDVAPNodeTRXThread::processNetworkFrame(unsigned char* data, unsigned int length, unsigned char seqNo)
{
	wxASSERT(data != NULL);
	wxASSERT(length > 0U);

	if (m_state != DSRS_NETWORK)
		return 0U;

	bool end = (seqNo & 0x40U) == 0x40U;
	if (end) {
		if (length > DV_FRAME_LENGTH_BYTES) {
			m_networkQueue[m_writeNum]->addData(data, DV_FRAME_LENGTH_BYTES, false);
			m_networkQueue[m_writeNum]->addData(data + DV_FRAME_LENGTH_BYTES, DV_FRAME_LENGTH_BYTES, true);
		} else {
			m_networkQueue[m_writeNum]->addData(data, DV_FRAME_LENGTH_BYTES, true);
		}

		endOfNetworkData();
		return 1U;
	}

	// Mask out the control bits of the sequence number
	seqNo &= 0x1FU;

	// Count the number of silence frames to insert
	unsigned int tempSeqNo = m_networkSeqNo;
	unsigned int count = 0U;
	while (seqNo != tempSeqNo) {
		count++;

		tempSeqNo++;
		if (tempSeqNo >= 21U)
			tempSeqNo = 0U;
	}

	// If the number is too high, then it probably means an old out-of-order frame, ignore it
	if (count > 18U)
		return 0U;

	unsigned int packetCount = 0U;

	// Insert missing frames
	while (seqNo != m_networkSeqNo) {
		unsigned char buffer[DV_FRAME_LENGTH_BYTES];
		if (count > SILENCE_THRESHOLD) {
			::memcpy(buffer, NULL_FRAME_DATA_BYTES, DV_FRAME_LENGTH_BYTES);
		} else {
			::memcpy(buffer, m_lastData, DV_FRAME_LENGTH_BYTES);
			m_ambe.regenerate(buffer);
			blankDTMF(buffer);
		}

		if (m_networkSeqNo == 0U)
			::memcpy(buffer + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES);

		m_networkQueue[m_writeNum]->addData(buffer, DV_FRAME_LENGTH_BYTES, false);

		packetCount++;
		m_networkSeqNo++;
		m_packetSilence++;
		if (m_networkSeqNo >= 21U)
			m_networkSeqNo = 0U;
	}

	// Regenerate the sync bytes
	if (m_networkSeqNo == 0U)
		::memcpy(data + VOICE_FRAME_LENGTH_BYTES, DATA_SYNC_BYTES, DATA_FRAME_LENGTH_BYTES);

	packetCount++;
	m_networkSeqNo++;
	if (m_networkSeqNo >= 21U)
		m_networkSeqNo = 0U;

	m_ambe.regenerate(data);
	blankDTMF(data);

	m_networkQueue[m_writeNum]->addData(data, DV_FRAME_LENGTH_BYTES, false);

	return packetCount;
}

void CDVAPNodeTRXThread::endOfRadioData()
{
	switch (m_state) {
		case DSRS_VALID:
			wxLogMessage(wxT("AMBE for %s  Frames: %.1fs, Silence: %.1f%%, BER: %.1f%%"), m_rxHeader->getMyCall1().c_str(), float(m_ambeFrames) / 50.0F, float(m_ambeSilence * 100U) / float(m_ambeFrames), float(m_ambeErrors * 100U) / float(m_ambeBits));

			if (m_tempAckText.IsEmpty()) {
				if (m_ack == AT_BER) {
					// Create the ack text with the linked reflector and BER
					wxString ackText;
					if (m_linkStatus == LS_LINKED_DEXTRA || m_linkStatus == LS_LINKED_DPLUS || m_linkStatus == LS_LINKED_DCS || m_linkStatus == LS_LINKED_CCS || m_linkStatus == LS_LINKED_LOOPBACK)
						ackText.Printf(wxT("%-8s  BER: %.1f%%   "), m_reflector.c_str(), float(m_ambeErrors * 100U) / float(m_ambeBits));
					else
						ackText.Printf(wxT("BER: %.1f%%            "), float(m_ambeErrors * 100U) / float(m_ambeBits));
					m_ackEncoder.setTextData(ackText);
				} else {
					m_ackEncoder.setTextData(m_ackText);
				}
			} else {
				m_ackEncoder.setTextData(m_tempAckText);
				m_tempAckText.Clear();
			}

			if (m_ack != AT_NONE)
				setRepeaterState(DSRS_VALID_WAIT);
			else
				setRepeaterState(DSRS_LISTENING);
			break;

		case DSRS_INVALID:
			wxLogMessage(wxT("AMBE for %s  Frames: %.1fs, Silence: %.1f%%, BER: %.1f%%"), m_rxHeader->getMyCall1().c_str(), float(m_ambeFrames) / 50.0F, float(m_ambeSilence * 100U) / float(m_ambeFrames), float(m_ambeErrors * 100U) / float(m_ambeBits));

			if (m_ack != AT_NONE)
				setRepeaterState(DSRS_INVALID_WAIT);
			else
				setRepeaterState(DSRS_LISTENING);
			break;

		case DSRS_TIMEOUT:
			wxLogMessage(wxT("AMBE for %s  Frames: %.1fs, Silence: %.1f%%, BER: %.1f%%"), m_rxHeader->getMyCall1().c_str(), float(m_ambeFrames) / 50.0F, float(m_ambeSilence * 100U) / float(m_ambeFrames), float(m_ambeErrors * 100U) / float(m_ambeBits));

			if (m_tempAckText.IsEmpty()) {
				if (m_ack == AT_BER) {
					// Create the ack text with the linked reflector and BER
					wxString ackText;
					if (m_linkStatus == LS_LINKED_DEXTRA || m_linkStatus == LS_LINKED_DPLUS || m_linkStatus == LS_LINKED_DCS || m_linkStatus == LS_LINKED_CCS || m_linkStatus == LS_LINKED_LOOPBACK)
						ackText.Printf(wxT("%-8s  BER: %.1f%%   "), m_reflector.c_str(), float(m_ambeErrors * 100U) / float(m_ambeBits));
					else
						ackText.Printf(wxT("BER: %.1f%%            "), float(m_ambeErrors * 100U) / float(m_ambeBits));
					m_ackEncoder.setTextData(ackText);
				} else {
					m_ackEncoder.setTextData(m_ackText);
				}
			} else {
				m_ackEncoder.setTextData(m_tempAckText);
				m_tempAckText.Clear();
			}

			if (m_ack != AT_NONE)
				setRepeaterState(DSRS_TIMEOUT_WAIT);
			else
				setRepeaterState(DSRS_LISTENING);
			break;

		default:
			break;
	}
}

void CDVAPNodeTRXThread::endOfNetworkData()
{
	if (m_watchdogTimer.hasExpired()) {
		m_packetCount   -= 77U;
		m_packetSilence -= 77U;
	}

	float loss = 0.0F;
	if (m_packetCount != 0U)
		loss = float(m_packetSilence) / float(m_packetCount);

	wxLogMessage(wxT("Stats for %s  Frames: %.1fs, Loss: %.1f%%, Packets: %u/%u"), m_rxHeader->getMyCall1().c_str(), float(m_packetCount) / 50.0F, loss * 100.0F, m_packetSilence, m_packetCount);

	setRepeaterState(DSRS_LISTENING);

	m_writeNum++;
	if (m_writeNum >= NETWORK_QUEUE_COUNT)
		m_writeNum = 0U;
}

CDVAPNodeStatusData* CDVAPNodeTRXThread::getStatus()
{
	if (m_state == DSRS_LISTENING) {
		return new CDVAPNodeStatusData(wxEmptyString, wxEmptyString, wxEmptyString, wxEmptyString,
				wxEmptyString, 0x00, 0x00, 0x00, m_tx, m_squelch, m_signal, m_state, m_timeoutTimer.getTimer(),
				m_timeoutTimer.getTimeout(), m_beaconTimer.getTimer(), m_beaconTimer.getTimeout(),
				m_announcementTimer.getTimer(), m_announcementTimer.getTimeout(), 0.0F, m_ackText);
	} else if (m_state == DSRS_NETWORK) {
		float loss = 0.0F;
		if (m_packetCount != 0U)
			loss = float(m_packetSilence) / float(m_packetCount);

		return new CDVAPNodeStatusData(m_rxHeader->getMyCall1(), m_rxHeader->getMyCall2(),
				m_rxHeader->getYourCall(), m_rxHeader->getRptCall1(), m_rxHeader->getRptCall2(), 
				m_rxHeader->getFlag1(), m_rxHeader->getFlag2(), m_rxHeader->getFlag3(), m_tx, m_squelch, m_signal,
				m_state, m_timeoutTimer.getTimer(), m_timeoutTimer.getTimeout(), m_beaconTimer.getTimer(),
				m_beaconTimer.getTimeout(), m_announcementTimer.getTimer(), m_announcementTimer.getTimeout(),
				loss * 100.0F, m_ackText);
	} else {
		float   bits = float(m_ambeBits - m_lastAMBEBits);
		float errors = float(m_ambeErrors - m_lastAMBEErrors);
		if (bits == 0.0F)
			bits = 1.0F;

		m_lastAMBEBits   = m_ambeBits;
		m_lastAMBEErrors = m_ambeErrors;

		return new CDVAPNodeStatusData(m_rxHeader->getMyCall1(), m_rxHeader->getMyCall2(),
				m_rxHeader->getYourCall(), m_rxHeader->getRptCall1(), m_rxHeader->getRptCall2(), 
				m_rxHeader->getFlag1(), m_rxHeader->getFlag2(), m_rxHeader->getFlag3(), m_tx, m_squelch, m_signal,
				m_state, m_timeoutTimer.getTimer(), m_timeoutTimer.getTimeout(), m_beaconTimer.getTimer(),
				m_beaconTimer.getTimeout(), m_announcementTimer.getTimer(), m_announcementTimer.getTimeout(),
				(errors * 100.0F) / bits, m_ackText);
	}
}

void CDVAPNodeTRXThread::clock(unsigned int ms)
{
	m_pollTimer.clock(ms);
	m_timeoutTimer.clock(ms);
	m_watchdogTimer.clock(ms);
	m_ackTimer.clock(ms);
	m_beaconTimer.clock(ms);
	m_announcementTimer.clock(ms);
	m_dvapPollTimer.clock(ms);
	if (m_beacon != NULL)
		m_beacon->clock();
	if (m_announcement != NULL)
		m_announcement->clock();
}

bool CDVAPNodeTRXThread::checkAnnouncements(const CHeaderData& header)
{
	if (m_announcement == NULL)
		return false;

	if (m_recordRPT1.IsSameAs(header.getRptCall1()) && m_recordRPT2.IsSameAs(header.getRptCall2())) {
		wxLogMessage(wxT("Announcement creation requested by %s/%s"), header.getMyCall1().c_str(), header.getMyCall2().c_str());
		m_announcement->writeHeader(header);
		m_recording = true;
		return true;
	}

	if (m_deleteRPT1.IsSameAs(header.getRptCall1()) && m_deleteRPT2.IsSameAs(header.getRptCall2())) {
		wxLogMessage(wxT("Announcement deletion requested by %s/%s"), header.getMyCall1().c_str(), header.getMyCall2().c_str());
		m_announcement->deleteAnnouncement();
		m_deleting = true;
		return true;
	}

	return false;
}

TRISTATE CDVAPNodeTRXThread::checkHeader(CHeaderData& header)
{
	// If not in RPT1 validation mode, then a simplex header is converted to a proper repeater header
	if (!m_rpt1Validation) {
		if (!header.isRepeaterMode()) {
			// Convert to a properly addressed repeater packet
			header.setRepeaterMode(true);
			header.setRptCall1(m_rptCallsign);
			header.setRptCall2(m_gwyCallsign);
		}
	}

	// The repeater bit must be set
	if (!header.isRepeaterMode()) {
		wxLogMessage(wxT("Received a non-repeater packet, ignoring"));
		return STATE_FALSE;
	}

	wxString my = header.getMyCall1();

	// Make sure MyCall is not empty, a silly value, or the repeater or gateway callsigns
	if (my.IsSameAs(m_rptCallsign) ||
		my.IsSameAs(m_gwyCallsign) ||
		my.Left(6U).IsSameAs(wxT("NOCALL")) ||
		my.Left(6U).IsSameAs(wxT("N0CALL")) ||
		my.Left(6U).IsSameAs(wxT("MYCALL"))) {
		wxLogMessage(wxT("Invalid MYCALL value of %s, ignoring"), my.c_str());
		return STATE_UNKNOWN;
	}

	// Check for a French class 3 novice callsign, and reject
	// Of the form F0xxx
	if (my.Left(2U).IsSameAs(wxT("F0"))) {
		wxLogMessage(wxT("French novice class licence callsign found, %s, ignoring"), my.c_str());
		return STATE_UNKNOWN;
	}

	// Check for an Australian foundation class licence callsign, and reject
	// Of the form VKnFxxx
	if (my.Left(2U).IsSameAs(wxT("VK")) && my.GetChar(3U) == wxT('F') && my.GetChar(6U) != wxT(' ')) {
		wxLogMessage(wxT("Australian foundation class licence callsign found, %s, ignoring"), my.c_str());
		return STATE_UNKNOWN;
	}

	// Check the MyCall value against the regular expression
	bool ok = m_regEx.Matches(my);
	if (!ok) {
		wxLogMessage(wxT("Invalid MYCALL value of %s, ignoring"), my.c_str());
		return STATE_UNKNOWN;
	}

	// Is it for us?
	if (!header.getRptCall1().IsSameAs(m_rptCallsign)) {
		wxLogMessage(wxT("Invalid RPT1 value %s, ignoring"), header.getRptCall1().c_str());
		return STATE_FALSE;
	}

	// If using callsign restriction, validate the my callsign
	if (m_restriction) {
		if (!my.Left(LONG_CALLSIGN_LENGTH - 1U).IsSameAs(m_rptCallsign.Left(LONG_CALLSIGN_LENGTH - 1U))) {
			wxLogMessage(wxT("Unauthorised user %s tried to access the repeater"), my.c_str());
			return STATE_UNKNOWN;
		}
	}

	return STATE_TRUE;
}

void CDVAPNodeTRXThread::blankDTMF(unsigned char* data)
{
	wxASSERT(data != NULL);

	// DTMF begins with these byte values
	if ((data[0] & DTMF_MASK[0]) == DTMF_SIG[0] && (data[1] & DTMF_MASK[1]) == DTMF_SIG[1] &&
		(data[2] & DTMF_MASK[2]) == DTMF_SIG[2] && (data[3] & DTMF_MASK[3]) == DTMF_SIG[3] &&
		(data[4] & DTMF_MASK[4]) == DTMF_SIG[4] && (data[5] & DTMF_MASK[5]) == DTMF_SIG[5] &&
		(data[6] & DTMF_MASK[6]) == DTMF_SIG[6] && (data[7] & DTMF_MASK[7]) == DTMF_SIG[7] &&
		(data[8] & DTMF_MASK[8]) == DTMF_SIG[8])
		::memcpy(data, NULL_AMBE_DATA_BYTES, VOICE_FRAME_LENGTH_BYTES);
}
