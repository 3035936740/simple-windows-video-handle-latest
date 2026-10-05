#include "engine.h"
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
using namespace svh;
void Check(bool condition,const char* description){if(!condition) throw std::runtime_error(description);}
int wmain(int argc,wchar_t** argv) {
    if(argc>1 && std::wstring(argv[1])==L"--echo") {
        for(int i=2;i<argc;++i) std::cout<<Utf8(argv[i])<<"\n";return 0;
    }
    try {
        std::vector<std::wstring> args={L"",L"C:\\中文 目录\\",L"a\"b",L"two\\\\\"quotes",L"a&b|c%PATH%;$(noop)",L"trailing\\",L"第一行\n第二行"};
        std::wstring command=L"executable";
        for(const auto& a:args) command+=L" "+QuoteArgument(a);
        int count=0;LPWSTR* parsed=CommandLineToArgvW(command.c_str(),&count);
        Check(parsed && count==static_cast<int>(args.size()+1),"argument count");
        for(size_t i=0;i<args.size();++i) Check(parsed[i+1]==args[i],"Windows quoting round trip");LocalFree(parsed);
        std::atomic<bool> stop{false};std::vector<std::wstring> childArgs={L"--echo"};childArgs.insert(childArgs.end(),args.begin(),args.end());
        auto result=RunProcess(ExeDirectory()/L"core_tests.exe",childArgs,stop);std::string expected;
        for(const auto& a:args) { auto text=Utf8(a); for(char c:text) { if(c=='\n') expected+='\r'; expected+=c; } expected+="\r\n"; }
        Check(result.code==0 && result.output==expected,"CreateProcess UTF-8/path argument round trip");
        stop=true;auto cancelled=RunProcess(ExeDirectory()/L"core_tests.exe",{L"--echo"},stop);
        Check(cancelled.cancelled,"pre-cancel must not launch subprocess");
        auto utf=std::wstring(L"中文水印 % : ' \\");Check(Wide(Utf8(utf))==utf,"UTF-8 round trip");
        std::cout<<"Core process and Unicode tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
