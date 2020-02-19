/****************************************************************************************************

 RepRapFirmware - Head

 Version 1.0

 Created on: Jan 23, 2020

 Matej Supik
 CSYSTEM CZ a.s.

 Licence: GPL

 ****************************************************************************************************/

#include "Head.h"

#include "GCodes/GCodes.h"
#include "Platform.h"
#include "RepRap.h"

Head * Head::freelist = nullptr;

// Create a new tool and return a pointer to it. If an error occurs, put an error message in 'reply' and return nullptr.
/*static*/ Head *Head::Create(unsigned int headNumber, const char *name, const char *configFileName, const StringRef& reply)
{
	Head *h;
	{
		TaskCriticalSectionLocker lock;
		h = freelist;
		if (h != nullptr)
		{
			freelist = h->next;
		}
	}

	if (h == nullptr)
	{
		h = new Head;
	}

	h->number = (uint16_t)headNumber;

	const size_t nameLength = strlen(name);
	if (nameLength != 0)
	{
		char *headName = new char[nameLength + 1];
		SafeStrncpy(headName, name, nameLength + 1);
		h->name = headName;
	}
	else
	{
		h->name = nullptr;
	}

	const size_t configFileNameLength = strlen(configFileName);
	if (configFileNameLength != 0)
	{
		char *headConfigFileName = new char[configFileNameLength + 1];
		SafeStrncpy(headConfigFileName, configFileName, configFileNameLength + 1);
		h->configFileName = headConfigFileName;
	}
	else
	{
		const size_t defaultConfigFileNameLength = strlen(HEAD_DEFAULT_CONFIG_FILE);

		char *headConfigFileName = new char[defaultConfigFileNameLength + 1];
		SafeStrncpy(headConfigFileName, HEAD_DEFAULT_CONFIG_FILE, defaultConfigFileNameLength + 1);
		h->configFileName = headConfigFileName;
	}

	h->next = nullptr;

	return h;
}

/*static*/ void Head::Delete(Head *h)
{
	if (h != nullptr)
	{
		delete h->name;
		h->name = nullptr;

		TaskCriticalSectionLocker lock;
		h->next = freelist;
		freelist = h;
	}
}

void Head::Activate()
{

}

// Write the tool's settings to file returning true if successful. The settings written leave the tool selected unless it is off.
bool Head::WriteSettings(FileStore *f) const
{
	String<FormatStringLength> buf;
	bool ok = true;

	if (ok)
	{
		ok = buf.printf("M1810 P%d S\"%s\" C\"%s\"", number, name, configFileName);
	}

	buf.cat('\n');
	ok = f->Write(buf.c_str());

	return ok;
}

void Head::Print(const StringRef& reply) const
{
	reply.printf("Head %u - name: %s, config: %s", number, GetName(), GetConfigFileName());
}

// End
