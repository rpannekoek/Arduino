#ifndef HTMLWRITER_H
#define HTMLWRITER_H

#include <StringBuilder.h>
#include <Navigation.h>

class HtmlWriter
{
    public:
        // Constructor
        HtmlWriter(StringBuilder& output, PGM_P icon, PGM_P css, size_t maxBarLength);

        // Destructor
        ~HtmlWriter();

        void setTitlePrefix(const String& prefix);

        void writeHeader(const String& title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval = 0);
        void writeHeader(const String& title, const Navigation& navigation, uint16_t refreshInterval = 0);
        void writeFooter();

        void writeBar(float value, const String& cssClass, bool fill, bool useDiv = true, size_t maxBarLength = 0);
        void writeStackedBar(float value1, float value2, const String& cssClass1, const String& cssClass2, bool fill, bool useDiv = true);
        void writeGraphCell(float value, const String& barCssClass, bool fill, size_t maxBarLength = 0);
        void writeGraphCell(float value1, float value2, const String& barCssClass1, const String& barCssClass2, bool fill);

        void writeFormStart(const String& action);
        void writeFormEnd();
        void writeSubmitButton();
        void writeTextBox(const String& name, const String& label, const String& value, uint16_t maxLength, const String& type = String("text"));
        void writeCheckbox(const String& name, const String& label, bool value);
        void writeRadioButtons(const String& name, const String& label, const char** values, int numValues, int index);
        void writeSlider(const String& name, const String& label, const String& unitOfMeasure, int value, int minValue, int maxValue, int denominator = 1);

        void writeHeading(const String& title, int level = 1);
        void writeTableStart();
        void writeTableEnd();
        void writeRowStart();
        void writeRowEnd();
        void writeCellStart(const String& cssClass);
        void writeCellEnd();

        void writeHeaderCell(const String& value, int colspan = 0, int rowspan = 0);
        void writeCell(const String& value);
        void writeCell(const char* value);
        void writeCell(int value);
        void writeCell(uint32_t value);
        void writeCell(float value, const __FlashStringHelper* format = nullptr);

        void writePager(int totalPages, int currentPage);

        void writeParagraph(const String& innerHtml);

        void writeActionLink(
            const String& action,
            const String& label,
            time_t currentTime,
            const String& cssClass= String("actionLink"),
            const String& icon= String());

    private:
        StringBuilder& _output;
        String _icon;
        String _css;
        String _titlePrefix;
        char* _bar;
        size_t _maxBarLength;
};

#endif