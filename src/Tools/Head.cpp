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
/*static*/ Head *Head::Create(unsigned int headNumber, const char *name, const StringRef& reply)
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

	h->next = nullptr;
	h->number = (uint16_t)headNumber;

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
		ok = buf.printf("M1810 P%d S\"%s\"", number, name);
	}

	buf.cat('\n');
	ok = f->Write(buf.c_str());

	return ok;
}

void Head::Print(const StringRef& reply) const
{
	reply.printf("Head %u - ", number);
	if (name != nullptr)
	{
		reply.catf("name: %s; ", name);
	}
}

// End
