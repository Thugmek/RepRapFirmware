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
constexpr size_t HeadConfigFileNameLength = 32;						// maximum allowed length for head config filename

class Head
{
public:

	static Head *Create(unsigned int headNumber, const char *name, const char *configFileName, const StringRef& reply);
	static void Delete(Head *h);
	Head *Next() const { return next; }

	const char *GetName() const;
	int GetNumber() const;
	const char *GetConfigFileName() const;

	friend class RepRap;

protected:
	void Activate();

	bool WriteSettings(FileStore *f) const;
	void Print(const StringRef& reply) const;

private:
	static Head *freelist;

	Head() : next(nullptr), name(nullptr), configFileName(nullptr) { }

	Head* next;
	int number;
	const char *name;
	const char *configFileName;
};

inline int Head::GetNumber() const
{
	return number;
}

inline const char *Head::GetName() const
{
	return (name == nullptr) ? "" : name;
}

inline const char *Head::GetConfigFileName() const
{
	return (configFileName == nullptr) ? HEAD_DEFAULT_CONFIG_FILE : configFileName;
}

#endif /* HEAD_H_ */
