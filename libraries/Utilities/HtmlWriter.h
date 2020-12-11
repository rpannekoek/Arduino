#ifndef HTMLWRITER_H
#define HTMLWRITER_H

#include <StringBuilder.h>

class HtmlWriter
{
    public:
        // Constructor
        HtmlWriter(StringBuilder& output, const char* icon, size_t maxBarLength);

        // Destructor
        ~HtmlWriter();

        void setTitlePrefix(const char* prefix);

        void writeHeader(String title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval = 0);
        void writeFooter();
        void writeBar(float value, String cssClass, bool fill);
        void writeTextBox(String name, String label, String value, uint16_t maxLength);
        void writeCheckbox(String name, String label, bool value);


    private:
        StringBuilder& _output;
        const char* _icon;
        const char* _titlePrefix;
        char* _bar;
        size_t _maxBarLength;
};

#endif