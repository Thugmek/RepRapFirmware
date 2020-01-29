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
/*static*/ Pad *Pad::Create(unsigned int padNumber, const char *name, const StringRef& reply)
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

	p->next = nullptr;
	p->number = (uint16_t)padNumber;

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
		ok = buf.printf("M1820 P%d S\"%s\"", number, name);
	}

	buf.cat('\n');
	ok = f->Write(buf.c_str());

	return ok;
}

void Pad::Print(const StringRef& reply) const
{
	reply.printf("Pad %u - ", number);
	if (name != nullptr)
	{
		reply.catf("name: %s; ", name);
	}
}

// End
