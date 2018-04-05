#ifndef __string_ext_h__
#define __string_ext_h__

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/**
 * Practice of string compare.
 *
 * char *strstr(const char *, const char *); 
 *            Locate substring
 *            Returns a pointer to the first occurrence of str2 in str1,
 *            or a null pointer if str2 is not part of str1.
 * bool startsWith(char* base, char* str);
 *            Custom function for detecting whether base is starts with str
 * bool endsWith(char* base, char* str);
 *            Custom function for detecting whether base is ends with str
 * int indexOf(char* base, char* str)
 *            Custom function for getting the first index of str in base
 *            -1 denotes not found
 * int indexOf_shift(char* base, char* str, int startIndex)
 *            Custom function for getting the first index of str in base
 *            after the given startIndex
 *            -1 denotes not found
 * int lastIndexOf(char* base, char* str)
 *            Custom function for getting the last index of str in base
 *            -1 denotes not found
 * References: 
 *        http://www.cplusplus.com/reference/cstring/strstr/
 * Source:
 *        http://ben-bai.blogspot.com.br/2013/03/c-string-startswith-endswith-indexof.html
 */

/** detecting whether base is starts with str **/
bool startsWith(char* base, char* str);

/** detecting whether base is ends with str **/
bool endsWith(char* base, char* str);

/** detecting whether base is equal with str **/
bool strEquals(char* base, char* str, bool check_size);

/** getting the first index of str in base **/
int indexOf(char* base, char* str);
int indexOf_shift(char* base, char* str, int startIndex);

/**
 * use two index to search in two part to prevent the worst case
 * (assume search 'aaa' in 'aaaaaaaa', you cannot skip three char each time)
 **/
int lastIndexOf(char* base, char* str);

#endif