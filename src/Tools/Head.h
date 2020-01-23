/****************************************************************************************************

RepRapFirmware - Head

Version 1.0

Created on: Jan 23, 2020

Matej Supik
CSYSTEM CZ a.s.

Licence: GPL

****************************************************************************************************/

#ifndef HEAD_H_
#define HEAD_H_

#include "RepRapFirmware.h"

constexpr size_t HeadNameLength = 32;						// maximum allowed length for head names

class Head
{
public:

	static Head *Create(unsigned int headNumber, const char *name, const StringRef& reply);
	static void Delete(Head *h);
	Head *Next() const { return next; }

	const char *GetName() const;
	int GetNumber() const;

	friend class RepRap;

protected:
	void Activate();

	bool WriteSettings(FileStore *f) const;
	void Print(const StringRef& reply) const;

private:
	static Head *freelist;

	Head() : next(nullptr), name(nullptr) { }

	Head* next;
	const char *name;
	int number;
};

inline const char *Head::GetName() const
{
	return (name == nullptr) ? "" : name;
}

inline int Head::GetNumber() const
{
	return number;
}

#endif /* HEAD_H_ */
