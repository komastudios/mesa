use std::iter;

use crate::args::fn_tuple_to_arr;
use crate::args::RawArg;
use crate::display_op::DisplayTokens;
use proc_macro2::{Span, TokenStream};
use syn::*;

#[derive(Default, Debug)]
pub struct ModifierArgs {
    pub name: Option<LitStr>,
    pub name_false: Option<LitStr>,
    pub def: Option<Type>,
}

impl syn::parse::Parse for ModifierArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = ModifierArgs::default();

        if input.is_empty() {
            return Ok(args);
        }

        let unhandled_err = |span: Span| {
            return Err(syn::Error::new(span, "Unhandled argument"))
                as syn::Result<()>;
        };

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::Literal(name) => {
                    if args.name.is_none() {
                        args.name = Some(name.clone());
                    } else if args.name_false.is_none() {
                        args.name_false = Some(name.clone())
                    } else {
                        unhandled_err(name.span())?;
                    }
                }
                RawArg::AssignType(d, def) if d == "def" => {
                    args.def
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.def = Some(def.clone())
                }
                x => unhandled_err(x.span())?,
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
pub enum ModifierType {
    BoolMod {
        name: LitStr,
        name_false: Option<LitStr>,
    },
    EnumMod {
        def: Option<Type>,
        ty: Type,
    },
}

impl ModifierType {
    fn is_optional(&self) -> bool {
        match self {
            ModifierType::BoolMod { name_false, .. } => name_false.is_none(),
            ModifierType::EnumMod { def, .. } => def.is_some(),
        }
    }
}

pub struct Modifier {
    pub ident: Ident,
    pub array_len: usize,
    pub ty: ModifierType,
}

impl Modifier {
    fn parse_field(field: &Field) -> syn::Result<Option<Self>> {
        let Some(attr) = field
            .attrs
            .iter()
            .filter(|x| x.path().is_ident("modifier"))
            .next()
        else {
            return Ok(None);
        };

        let is_type_bool = |ty: &Type| {
            matches!(ty, Type::Path(TypePath {
            qself: None,
            path
        }) if path.is_ident("bool"))
        };

        let (array_len, ty) = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len:
                    Expr::Lit(ExprLit {
                        lit: Lit::Int(len), ..
                    }),
                ..
            }) => (len.base10_parse()?, &elem as &Type),
            ty => (0usize, ty),
        };
        let is_bool = is_type_bool(&ty);

        let args: ModifierArgs = match attr.meta {
            Meta::Path(_) => ModifierArgs::default(),
            _ => attr.parse_args()?,
        };
        let ident = field.ident.as_ref().unwrap().clone();
        let mod_ty = if is_bool {
            ModifierType::BoolMod {
                name_false: args.name_false,
                name: args.name.unwrap_or_else(|| {
                    let ident = field.ident.as_ref().unwrap();
                    let fname = ident.to_string();
                    LitStr::new(&format!(".{fname}"), ident.span())
                }),
            }
        } else {
            ModifierType::EnumMod {
                def: args.def.clone(),
                ty: ty.clone(),
            }
        };
        Ok(Some(Modifier {
            ident,
            array_len,
            ty: mod_ty,
        }))
    }

    pub fn parse_all(data: &DataStruct) -> syn::Result<Vec<Self>> {
        data.fields
            .iter()
            .filter_map(|x| Self::parse_field(x).transpose())
            .collect()
    }
}

impl quote::ToTokens for DisplayTokens<&Modifier> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Modifier {
            ident,
            array_len,
            ty,
        } = self.0;

        let generate_no_arr = |ident| match ty {
            ModifierType::BoolMod {
                name,
                name_false: None,
            } => {
                quote! {
                    if #ident {
                        write!(f, #name)?;
                    }
                }
            }
            ModifierType::BoolMod {
                name,
                name_false: Some(name_false),
            } => {
                quote! {
                    write!(f, "{}", if #ident { #name } else { #name_false })?;
                }
            }
            ModifierType::EnumMod { def: None, .. } => {
                quote! {
                    write!(f, "{}", #ident)?;
                }
            }
            ModifierType::EnumMod { def: Some(def), .. } => {
                quote! {
                    if #ident != #def {
                        write!(f, "{}", #ident)?;
                    }
                }
            }
        };
        let t = match array_len {
            0 => generate_no_arr(quote! { self.#ident }),
            n => (0..*n)
                .map(|i| generate_no_arr(quote! { self.#ident[#i]}))
                .collect(),
        };
        t.to_tokens(tokens);
    }
}

pub fn modifiers_to_parser_tokens(mods: &[Modifier]) -> TokenStream {
    // Anatomy of a parser
    // 1 Parser creation: create the parser that will match the input text
    // 2 OptionalPermutation(all mods): parser that matches any permutation of the parsers
    // 3 Result destructuring: destructure the results of parsing the permutations into Option<R> where R is the result
    // 4 Result processing: Transform the result from Option<R> to the real modifier
    // 5 Object creation
    if mods.is_empty() {
        return quote! { () };
    }

    let parser_tokens = mods.iter().map(|x| {
        let Modifier { array_len, ty, .. } = x;

        // THERE MUST BE NO OPTIONAL PARSERS!
        // Optional parsers break OptionalPermutation
        let single_tokens = match ty {
            ModifierType::BoolMod {
                name,
                name_false: None,
            } => quote! {
                crate::parser::tag(#name).map(|x| ())
            },
            ModifierType::BoolMod {
                name,
                name_false: Some(name_false),
            } => quote! {
                crate::parser::tag(#name).map(|_| true).or(
                    crate::parser::tag(#name_false).map(|_| false)
                )
            },
            ModifierType::EnumMod { ty, .. } => quote! {
                #ty::parse
            },
        };
        let t = match array_len {
            0 => single_tokens,
            n => {
                let ts = iter::repeat(single_tokens).take(*n);
                let map_fn = fn_tuple_to_arr(*n);
                quote! {
                    (#(#ts),*).and().map(#map_fn)
                }
            }
        };
        t as TokenStream
    });

    let remove_optional = |ident: &Ident, modif: &Modifier| {
        let Modifier { array_len, ty, .. } = modif;

        assert!(
            *array_len == 0 || !ty.is_optional(),
            "Optional modifier arrays not implemented yet"
        );
        match ty {
            ModifierType::BoolMod {
                name_false: None, ..
            } => quote! {
                #ident.is_some()
            },
            ModifierType::BoolMod {
                name,
                name_false: Some(name_false),
            } => {
                let err_str = format!(
                    "Missing {} or {}",
                    name.value(),
                    name_false.value()
                );
                quote! {
                    #ident.ok_or(crate::parser::ErrorKind::CustomErr(#err_str))?
                }
            }
            ModifierType::EnumMod { def: None, .. } => {
                let err_str = format!("Missing {ident} modifier");
                quote! {
                    #ident.ok_or(crate::parser::ErrorKind::CustomErr(#err_str))?
                }
            }
            ModifierType::EnumMod { def: Some(def), .. } => {
                quote! {
                    #ident.unwrap_or(#def)
                }
            }
        }
    };

    match mods.len() {
        1 => {
            if mods[0].ty.is_optional() {
                let remove_opt = remove_optional(&format_ident!("x"), &mods[0]);
                quote! { #(#parser_tokens)*.opt().map(|x| #remove_opt) }
            } else {
                quote! { #(#parser_tokens)* }
            }
        }
        _ => {
            let destructure = modifiers_to_destructure_tokens(mods);
            let map_tokens = mods.iter().map(|x| remove_optional(&x.ident, x));
            quote! {
                crate::parser::OptionalPermutation((#(#parser_tokens,)* )).and_then(move |#destructure| {
                    Ok((#(#map_tokens), *))
                })
            }
        }
    }
}

pub fn modifiers_to_destructure_tokens(mods: &[Modifier]) -> TokenStream {
    let tokens = mods.iter().map(|x| &x.ident);
    match mods.len() {
        0 => quote! { () },
        1 => quote! { #(#tokens)* },
        _ => quote! { (#(#tokens),*) },
    }
}
