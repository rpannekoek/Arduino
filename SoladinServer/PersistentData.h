#include <PersistentDataBase.h>


struct PersistentDataStruct : PersistentDataBase
{
    char hostName[32];

    PersistentDataStruct() 
        : PersistentDataBase(sizeof(hostName)) {}

    virtual void initialize()
    {
        strcpy(hostName, "SoladinServer");
    }

    virtual void validate()
    {
        // Ensure all strings are terminated
        hostName[sizeof(hostName) - 1] = 0;
    }
};

PersistentDataStruct PersistentData;
