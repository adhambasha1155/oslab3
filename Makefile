## Builds the mini shell: compiles command.cc and tokenizer.cc and links them
## into one executable called "myshell". Run `make`, then `./myshell`.

CXX      = g++
CXXFLAGS = -Wall -g -std=c++17

myshell: command.o tokenizer.o
	$(CXX) $(CXXFLAGS) -o myshell command.o tokenizer.o

command.o: command.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c command.cc

tokenizer.o: tokenizer.cc tokenizer.h
	$(CXX) $(CXXFLAGS) -c tokenizer.cc

clean:
	rm -f *.o myshell shell.log
