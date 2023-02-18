typedef uint8_t uuid128_t[16];

struct UUID128
{
    uuid128_t data;

    UUID128();
    UUID128(const UUID128& uuid);
    UUID128(const uuid128_t& uuid);
    UUID128(const String& uuid);

    String toString() const;

    bool equals(const UUID128& other) const;
    bool equals(const uuid128_t& other) const;
};