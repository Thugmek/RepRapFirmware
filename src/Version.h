/*
 * Version.h
 *
 *  Created on: 25 Dec 2016
 *      Author: David
 */

#ifndef SRC_VERSION_H_
#define SRC_VERSION_H_


#ifndef VERSION
#ifdef RTOS
# define MAIN_VERSION	"2.03trilab4"
#else
# define MAIN_VERSION	"1.24"
#endif

# define VERSION MAIN_VERSION
#endif

#ifndef DATE
# define DATE "2019-09-03"
#endif

#define AUTHORS "reprappro, dc42, chrishamm, t3p3, dnewman, printm3d, mat100"

#endif /* SRC_VERSION_H_ */
