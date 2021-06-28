/****************************************************************************************************

 RepRapFirmware - Pad

 Version 1.0

 Created on: Jan 23, 2020

 Matej Supik
 CSYSTEM CZ a.s.

 Licence: GPL

 ****************************************************************************************************/

#include "Pad.h"

#include "GCodes/GCodes.h"
#include "Platform.h"
#include "RepRap.h"

Pad * Pad::freelist = nullptr;

// Create a new tool and return a pointer to it. If an error occurs, put an error message in 'reply' and return nullptr.
/*static*/ Pad *Pad::Create(unsigned int padNumber, const char *name, const char *configFileName, const StringRef& reply)
{
	Pad *p;
	{
		TaskCriticalSectionLocker lock;
		p = freelist;
		if (p != nullptr)
		{
			freelist = p->next;
		}
	}

	if (p == nullptr)
	{
		p = new Pad;
	}

	p->number = (uint16_t)padNumber;

	const size_t nameLength = strlen(name);
	if (nameLength != 0)
	{
		char *padName = new char[nameLength + 1];
		SafeStrncpy(padName, name, nameLength + 1);
		p->name = padName;
	}
	else
	{
		p->name = nullptr;
	}

	const size_t configFileNameLength = strlen(configFileName);
	if (configFileNameLength != 0)
	{
		char *padConfigFileName = new char[configFileNameLength + 1];
		SafeStrncpy(padConfigFileName, configFileName, configFileNameLength + 1);
		p->configFileName = padConfigFileName;
	}
	else
	{
		const size_t defaultConfigFileNameLength = strlen(PAD_DEFAULT_CONFIG_FILE);

		char *padConfigFileName = new char[defaultConfigFileNameLength + 1];
		SafeStrncpy(padConfigFileName, PAD_DEFAULT_CONFIG_FILE, defaultConfigFileNameLength + 1);
		p->configFileName = padConfigFileName;
	}

	p->diameter = 0;

	p->next = nullptr;

	return p;
}

/*static*/ void Pad::Delete(Pad *p)
{
	if (p != nullptr)
	{
		delete p->name;
		p->name = nullptr;

		TaskCriticalSectionLocker lock;
		p->next = freelist;
		freelist = p;
	}
}

void Pad::Activate()
{

}

// Write the pad settings to file returning true if successful. The settings written leave the tool selected unless it is off.
bool Pad::WriteSettings(FileStore *f) const
{
	String<FormatStringLength> buf;
	bool ok = true;

	if (ok)
	{
		ok = buf.printf("M1820 P%d S\"%s\" C\"%s\"", number, name, configFileName);
	}

	buf.cat('\n');
	ok = f->Write(buf.c_str());

	return ok;
}

void Pad::Print(const StringRef& reply) const
{
	reply.printf("Pad %u - name: %s, config: %s", number, GetName(), GetConfigFileName());
}

void Pad::RestoreDefaultParameters()
{
	diameter = 0;
}

// End
