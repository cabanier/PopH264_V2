#define __export

typedef int int32_t;

__export const char* PopH264_GetVersion()
{
	/*
	auto Function = [&]()
	{
		return PopH264::Version.GetMillion();
	};
	return SafeCall( Function, __func__, -1 );
	*/
	return "1.3.32";
}
