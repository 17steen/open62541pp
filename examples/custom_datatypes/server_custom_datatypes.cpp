#include <map>
#include <thread>
#include <open62541pp/node.hpp>
#include <open62541pp/server.hpp>

#include "custom_datatypes.hpp"

#include "reflect.hpp"

static uint32_t current_datatype_node_id = 4000;
static uint32_t current_binary_encoding_node_id = 5000;
static std::map<std::string, std::pair<uint32_t, uint32_t>> datatype_node_ids;

static std::pair<uint32_t, uint32_t> get_datatype_node_id(const std::string_view& data_type_name) {
    if (!datatype_node_ids.contains(std::string{data_type_name}))
        datatype_node_ids[std::string{data_type_name}] = {++current_datatype_node_id, ++current_binary_encoding_node_id};

    return datatype_node_ids[std::string{data_type_name}];
}

template<typename T>
const opcua::DataType &get_custom_datatype_ref() {
    static const opcua::DataType dt = []() {
        const auto data_type_name = reflect::type_name(T{});
        const auto [data_type_id, binary_encoding] = get_datatype_node_id(data_type_name);
        const auto node_id = opcua::NodeId{1, data_type_id};
        const auto encoding_node_id = opcua::NodeId{1, binary_encoding};

        auto prev = opcua::DataTypeBuilder<T>::createStructure( data_type_name.data(), node_id, encoding_node_id);

        reflect::for_each<T>([&](auto I) {
            using TMember = std::decay_t<decltype(reflect::get<I>(T{}))>;
            prev = prev.template addFieldWithOffset<TMember>(reflect::member_name<I>(T{}).data(), reflect::offset_of<I>(T{}));
        });

        return prev.build();
    }();
    return dt;
}

template<typename NotOpcuaType>
struct opcua_compatible_t {

};


struct IntArray {
    size_t size;
    int* values;

    IntArray(const std::vector<int>& vec) {
        size = vec.size();
        values = new int[vec.size()];
        std::uninitialized_copy(vec.begin(), vec.end(), values);
    }
};

struct NestedStruct {
    opcua::String string;
    Point point;
};

namespace opcua {
template<>
struct TypeRegistry<Measurements>
{
    static const auto& getDataType() noexcept {
        return get_custom_datatype_ref<Measurements>();
    }
};

template<>
struct TypeRegistry<Point> {
    static const auto& getDataType() noexcept {
        return get_custom_datatype_ref<Point>();
    }
};

template<>
struct TypeRegistry<NestedStruct>
{
    static const auto& getDataType() noexcept {
        return get_custom_datatype_ref<NestedStruct>();
    }
};
}

template<typename T>
struct opcua_convertible {
    // // if T is convertible to
    // // opcua::detail::IsConvertibleType<T>
    // using type = opcua::TypeConverter<T>::NativeType;
    static_assert(sizeof(T) != sizeof(T), "Type not convertible to OPC UA type");
};

template<typename T> requires opcua::detail::isConvertibleType<T>
struct opcua_convertible<T> {
    using type = typename opcua::TypeConverter<T>::NativeType;
};

template<typename T> requires opcua::detail::isRegisteredType<T>
struct opcua_convertible<T> {
    using type = typename opcua::TypeRegistry<T>::NativeType;
};

template<typename T>
struct as_opcua_compatible {
    using source_type = T;
    using type = decltype( reflect::visit([]<typename ...Args>([[maybe_unused]] const Args&... args) { return std::tuple<typename opcua_convertible<Args>::type...>{}; }, std::declval<T>()));
};

struct NotOpcuaStruct {
    std::string normal_string;
    // std::vector<int> normal_vector;
};

static_assert(std::is_aggregate_v<NotOpcuaStruct>);

template<size_t Idx, class T>
static size_t tuple_element_offset() {
    const T* null_tuple = nullptr;
    const void *base_pointer = null_tuple;
    const void* member_pointer = &std::get<Idx>(*null_tuple);

    const char* base_pointer_as_char = static_cast<const char*>(base_pointer);
    const char* member_pointer_as_char = static_cast<const char*>(member_pointer);

    const ptrdiff_t offset = member_pointer_as_char - base_pointer_as_char;

    return static_cast<size_t>(offset);
}

template<size_t I, typename T>
struct zero_terminated_member_name {
    static constexpr auto member_name_str = reflect::member_name<I, T>();
    static constexpr auto fixed_size_member_name_str = reflect::fixed_string<char, member_name_str.size()>(member_name_str.data());
    static std::string_view get_member_name() {
        return {fixed_size_member_name_str};
    }
};


template<typename T>
const opcua::DataType &get_custom_datatype_ref_with_opcua_conversion() {
    using tuple_t = typename as_opcua_compatible<T>::type;

    static const opcua::DataType dt = []() {
        constexpr auto data_type_name = reflect::type_name<T>();
        const auto [data_type_id, binary_encoding] = get_datatype_node_id(data_type_name);
        const auto node_id = opcua::NodeId{1, data_type_id};
        const auto encoding_node_id = opcua::NodeId{1, binary_encoding};

        auto prev = opcua::DataTypeBuilder<tuple_t>::createStructure( data_type_name.data(), node_id, encoding_node_id);

        reflect::for_each<T>([&](auto I)  {
            using TTupleMember = std::tuple_element_t<I, tuple_t>;

            const auto& member_name = zero_terminated_member_name<I, T>::get_member_name();

            auto offset = tuple_element_offset<I, tuple_t>();

            prev = prev.template addFieldWithOffset<TTupleMember>(member_name.data(), offset);
        });

        return prev.build();
    }();
    return dt;
}

namespace opcua {
template<>
struct TypeRegistry<as_opcua_compatible<NotOpcuaStruct>::type>
{
    static const auto& getDataType() noexcept {
        return get_custom_datatype_ref_with_opcua_conversion<NotOpcuaStruct>();
    }
};

template<>
struct TypeConverter<NotOpcuaStruct> {
    using NativeType = as_opcua_compatible<NotOpcuaStruct>::type;
    using Type = NotOpcuaStruct;

    static void fromNative(const NativeType& src, Type& dst) {
        reflect::for_each<Type>([&](auto I) {
            using opcua_tuple_element_type = std::tuple_element_t<I, NativeType>;
            using native_type_member = reflect::member_type<I, Type>;

            const auto& tuple_element = std::get<I>(src);
            auto& member = reflect::get<I>(dst);

            if constexpr (std::is_convertible_v<opcua_tuple_element_type, native_type_member>) {
                member = tuple_element;
            }
            else if constexpr (detail::isConvertibleType<native_type_member>) {
                opcua::TypeConverter<native_type_member>::fromNative(tuple_element, member);
            }
            else {
                []<bool false_v = false>() { static_assert(false_v, "type cannot be converted"); }();
            }
        });
    }

    static void toNative(const Type& src, NativeType& dst) {
        reflect::for_each<Type>([&](auto I) {
            using opcua_tuple_element_type = std::tuple_element_t<I, NativeType>;
            using native_member_type = reflect::member_type<I, Type>;

            const auto& member = reflect::get<I>(src);
            auto& tuple_element = std::get<I>(dst);

            if constexpr (std::is_convertible_v<native_member_type, opcua_tuple_element_type>) {
                    tuple_element = member;
            }
            else if constexpr (detail::isConvertibleType<native_member_type>) {
                    opcua::TypeConverter<native_member_type>::toNative(member, tuple_element);
            }
            else {
                    []<bool false_v = false>() { static_assert(false_v, "type cannot be converted"); }();
            }
        });
    }
};
}


int main() {
    using opcua_tuple_t = as_opcua_compatible<NotOpcuaStruct>::type;


    static_assert(std::is_same_v<decltype(std::declval<NotOpcuaStruct>().normal_string), std::string>);
    static_assert(std::is_same_v<std::tuple_element_t<0, opcua_tuple_t>, opcua::String>);

    auto my_very_own_type = NotOpcuaStruct{.normal_string = "hey"};

    opcua::Server server;

    // Get custom type definitions from common header
    const auto &dataTypePoint = get_custom_datatype_ref<Point>();
    const auto &dataTypeMeasurements = get_custom_datatype_ref<Measurements>();
    const auto &dataTypeOpt = get_custom_datatype_ref<Opt>();
    const auto &dataTypeUni = getUniDataType();
    const auto &dataTypeColor = getColorDataType();
    const auto &dataTypeNested = get_custom_datatype_ref<NestedStruct>();
    const auto &opcua_tuple_type = opcua::asWrapper<opcua::DataType>(opcua::TypeRegistry<opcua_tuple_t>::getDataType());

    // Provide custom data type definitions to server
    server.config().addCustomDataTypes({
        dataTypePoint,
        dataTypeMeasurements,
        dataTypeOpt,
        dataTypeUni,
        dataTypeColor,
        dataTypeNested,
        opcua_tuple_type,
    });

    // Add data type nodes
    opcua::Node structureDataTypeNode(server, opcua::DataTypeId::Structure);
    structureDataTypeNode.addDataType(dataTypePoint.typeId(), "PointDataType");
    structureDataTypeNode.addDataType(dataTypeMeasurements.typeId(), "MeasurementsDataType");
    structureDataTypeNode.addDataType(dataTypeOpt.typeId(), "OptDataType");
    structureDataTypeNode.addDataType(dataTypeUni.typeId(), "UniDataType");
    structureDataTypeNode.addDataType(opcua_tuple_type.typeId(), "NotOpcuaStructDataType");
    structureDataTypeNode.addDataType(dataTypeNested.typeId(), "NestedStructDataType");
    opcua::Node enumerationDataTypeNode(server, opcua::DataTypeId::Enumeration);
    enumerationDataTypeNode.addDataType(dataTypeColor.typeId(), "Color")
        .addProperty(
            {0, 0},  // auto-generate node id
            "EnumValues",
            opcua::VariableAttributes{}
                .setDataType<opcua::EnumValueType>()
                .setValueRank(opcua::ValueRank::OneDimension)
                .setArrayDimensions({0})
                .setValueArray(opcua::Span<const opcua::EnumValueType>{
                    {0, {"", "Red"}, {}},
                    {1, {"", "Green"}, {}},
                    {2, {"", "Yellow"}, {}},
                })
            )
            .addModellingRule(opcua::ModellingRule::Mandatory);

    // Add variable type nodes (optional)
    opcua::Node baseDataVariableTypeNode(server, opcua::VariableTypeId::BaseDataVariableType);
    auto variableTypePointNode = baseDataVariableTypeNode.addVariableType(
        {1, 4243},
        "PointType",
        opcua::VariableTypeAttributes{}
            .setDataType(dataTypePoint.typeId())
            .setValueRank(opcua::ValueRank::ScalarOrOneDimension)
            .setValueScalar(Point{1, 2, 3}, dataTypePoint)
    );
    auto variableTypeMeasurementNode = baseDataVariableTypeNode.addVariableType(
        {1, 4444},
        "MeasurementsType",
        opcua::VariableTypeAttributes{}
            .setDataType(dataTypeMeasurements.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(Measurements{}, dataTypeMeasurements)
    );
    auto variableTypeOptNode = baseDataVariableTypeNode.addVariableType(
        {1, 4645},
        "OptType",
        opcua::VariableTypeAttributes{}
            .setDataType(dataTypeOpt.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(Opt{}, dataTypeOpt)
    );
    auto variableTypeUniNode = baseDataVariableTypeNode.addVariableType(
        {1, 4846},
        "UniType",
        opcua::VariableTypeAttributes{}
            .setDataType(dataTypeUni.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(Uni{}, dataTypeUni)
    );

    // Add variable nodes with some values
    opcua::Node objectsNode(server, opcua::ObjectId::ObjectsFolder);

    const Point point{3.0, 4.0, 5.0};
    auto my_point_node = objectsNode.addVariable(
        {1, "Point"},
        "Point",
        opcua::VariableAttributes{}
            .setDataType(dataTypePoint.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(point, dataTypePoint),
        variableTypePointNode.id()
    );

    auto my_nested_node = objectsNode.addVariable(
        {1, "NestedStruct"},
        "NestedStruct",
        opcua::VariableAttributes{}
            .setDataType(dataTypeNested.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(NestedStruct{.string = opcua::String{"hey"}, .point = point}, dataTypeNested)
    );

    auto my_special_type_node = objectsNode.addVariable(
        {1, "NotOpcuaStruct"},
        "NotOpcuaStruct",
        opcua::VariableAttributes{}
            .setDataType(opcua_tuple_type.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(NotOpcuaStruct{.normal_string = "not opcua string"})
        );

    const std::vector<Point> pointVec{{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
    objectsNode.addVariable(
        {1, "PointVec"},
        "PointVec",
        opcua::VariableAttributes{}
            .setDataType(dataTypePoint.typeId())
            .setArrayDimensions({0})  // single dimension but unknown in size
            .setValueRank(opcua::ValueRank::OneDimension)
            .setValueArray(pointVec, dataTypePoint),
        variableTypePointNode.id()
    );

    std::vector<float> measurementsValues{19.1F, 20.2F, 19.7F};
    const Measurements measurements{
        opcua::String("Test description"),
        measurementsValues.size(),
        measurementsValues.data(),
    };
    objectsNode.addVariable(
        {1, "Measurements"},
        "Measurements",
        opcua::VariableAttributes{}
            .setDataType(dataTypeMeasurements.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(measurements, dataTypeMeasurements),
        variableTypeMeasurementNode.id()
    );

    float optC = 10.10F;
    const Opt opt{3, nullptr, &optC};
    objectsNode.addVariable(
        {1, "Opt"},
        "Opt",
        opcua::VariableAttributes{}
            .setDataType(dataTypeOpt.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(opt, dataTypeOpt),
        variableTypeOptNode.id()
    );

    Uni uni{};
    uni.switchField = UniSwitch::OptionB;
    uni.fields.optionB = UA_STRING_STATIC("test string"); // NOLINT
    objectsNode.addVariable(
        {1, "Uni"},
        "Uni",
        opcua::VariableAttributes{}
            .setDataType(dataTypeUni.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(uni, dataTypeUni),
        variableTypeUniNode.id()
    );

    objectsNode.addVariable(
        {1, "Color"},
        "Color",
        opcua::VariableAttributes{}
            .setDataType(dataTypeColor.typeId())
            .setValueRank(opcua::ValueRank::Scalar)
            .setValueScalar(Color::Green, dataTypeColor)
    );

    auto opc_ua_server_thread = std::jthread([&server](){ server.run(); });

    const Point point2 = Point{4, 5, 6};
    my_point_node.writeValueScalar( point2);
    // const Point point2{4.0, 5.0, 6.0};
    // my_point_node.writeValueScalar(point2);
}
