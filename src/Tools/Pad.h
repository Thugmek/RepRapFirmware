/****************************************************************************************************

RepRapFirmware - Pad

Version 1.0

Created on: Jan 23, 2020

Matej Supik
CSYSTEM CZ a.s.

Licence: GPL

****************************************************************************************************/

#ifndef PAD_H_
#define PAD_H_

#include "RepRapFirmware.h"

constexpr size_t PadNameLength = 32;						// maximum allowed length for head names

class Pad
{
public:

	static Pad *Create(unsigned int padNumber, const char *name, const StringRef& reply);
	static void Delete(Pad *p);
	Pad *Next() const { return next; }

	const char *GetName() const;
	int GetNumber() const;

	friend class RepRap;

protected:
	void Activate();

	bool WriteSettings(FileStore *f) const;
	void Print(const StringRef& reply) const;

private:
	static Pad *freelist;

	Pad() : next(nullptr), name(nullptr) { }

	Pad* next;
	const char *name;
	int number;
};

inline const char *Pad::GetName() const
{
	return (name == nullptr) ? "" : name;
}

inline int Pad::GetNumber() const
{
	return number;
}

#endif /* PAD_H_ */
