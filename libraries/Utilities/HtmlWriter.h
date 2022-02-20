#ifndef HTMLWRITER_H
#define HTMLWRITER_H

#include <StringBuilder.h>

class HtmlWriter
{
    public:
        // Constructor
        HtmlWriter(StringBuilder& output, const char* icon, const char* css, size_t maxBarLength);

        // Destructor
        ~HtmlWriter();

        void setTitlePrefix(const char* prefix);

        void writeHeader(String title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval = 0);
        void writeFooter();
        void writeBar(float value, String cssClass, bool fill, bool useDiv = true, size_t maxBarLength = 0);
        void writeStackedBar(float value1, float value2, String cssClass1, String cssClass2, bool fill, bool useDiv = true);
        void writeTextBox(String name, String label, String value, uint16_t maxLength);
        void writeCheckbox(String name, String label, bool value);
        void writeRadioButtons(String name, String label, const char** values, int numValues, int index);
        void writeSlider(String name, String label, String unitOfMeasure, int value, int minValue, int maxValue, int denominator = 1);

    private:
        StringBuilder& _output;
        const char* _icon;
        const char* _css;
        const char* _titlePrefix;
        char* _bar;
        size_t _maxBarLength;
};

#endif