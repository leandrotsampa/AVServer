#include <string_ext.h>

bool startsWith(char* base, char* str)
{
	return (strstr(base, str) - base) == 0;
}

bool endsWith(char* base, char* str)
{
	int blen = strlen(base);
	int slen = strlen(str);

	return ((blen >= slen) && (0 == strcmp(base + blen - slen, str)));
}

bool strEquals(char* base, char* str, bool check_size)
{
	if (check_size)
		return !strncmp(base, str, strlen(str));
	else
		return !strcmp(base, str);
}

int indexOf(char* base, char* str)
{
	return indexOf_shift(base, str, 0);
}

int indexOf_shift(char* base, char* str, int startIndex)
{
	int result;
	int baselen = strlen(base);

	// str should not longer than base
	if (strlen(str) > baselen || startIndex > baselen)
		result = -1;
	else
	{
		if (startIndex < 0 )
			startIndex = 0;

		char* pos = strstr(base + startIndex, str);
		if (pos == NULL)
			result = -1;
		else
			result = pos - base;
	}

	return result;
}

int lastIndexOf(char* base, char* str)
{
	int result;

	// str should not longer than base
    if (strlen(str) > strlen(base))
        result = -1;
    else
	{
		int start = 0;
		int endinit = strlen(base) - strlen(str);
		int end = endinit;
		int endtmp = endinit;
		while(start != end)
		{
			start = indexOf_shift(base, str, start);
			end = indexOf_shift(base, str, end);

			// not found from start
			if (start == -1)
				end = -1; // then break;
			else if (end == -1)
			{
				// found from start
				// but not found from end
				// move end to middle
				if (endtmp == (start+1))
					end = start; // then break;
                else
				{
					end = endtmp - (endtmp - start) / 2;
					if (end <= start)
						end = start+1;
					endtmp = end;
				}
			}
			else
			{
				// found from both start and end
				// move start to end and
				// move end to base - strlen(str)
				start = end;
				end = endinit;
			}
		}

		result = start;
	}

	return result;
}
