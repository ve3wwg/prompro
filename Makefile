######################################################################
#  Makefile
######################################################################

-include Makefile.incl

CXX=c++

all:	prompro

OBJS	= prompro.o pugixml.o

prompro: $(OBJS)
	$(CXX) $(OBJS) -o prompro

clean:
	rm -f *.o 

clobber: clean
	rm -f prompro
	@rm -f errs.t .errs.t

# End
