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
# define MAIN_VERSION	"2.04trilab0"
#else
# define MAIN_VERSION	"1.25"
#endif

# define VERSION MAIN_VERSION
#endif

#ifndef DATE
# define DATE "2019-11-28"
#endif

#define AUTHORS "reprappro, dc42, chrishamm, t3p3, dnewman, printm3d, mat100"

#endif /* SRC_VERSION_H_ */
