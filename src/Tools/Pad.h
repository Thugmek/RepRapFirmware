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
constexpr size_t PadConfigFileNameLength = 32;				// maximum allowed length for head config filename

class Pad
{
public:

	static Pad *Create(unsigned int padNumber, const char *name, const char *configFileName, const StringRef& reply);
	static void Delete(Pad *p);
	Pad *Next() const { return next; }

	int GetNumber() const;
	const char *GetName() const;
	const char *GetConfigFileName() const;

	void RestoreDefaultParameters();

	void SetDiameter(const int d);
	int GetDiameter();

	friend class RepRap;

protected:
	void Activate();

	bool WriteSettings(FileStore *f) const;
	void Print(const StringRef& reply) const;

private:
	static Pad *freelist;

	Pad() : next(nullptr), name(nullptr), configFileName(nullptr) { }

	Pad* next;
	int number;
	const char *name;
	const char *configFileName;
	int diameter;
};

inline int Pad::GetNumber() const
{
	return number;
}

inline const char *Pad::GetName() const
{
	return (name == nullptr) ? "" : name;
}

inline const char *Pad::GetConfigFileName() const
{
	return (configFileName == nullptr) ? PAD_DEFAULT_CONFIG_FILE : configFileName;
}

inline void Pad::SetDiameter(const int d)
{
	diameter = d;
}

inline int Pad::GetDiameter()
{
	return diameter;
}

#endif /* PAD_H_ */
